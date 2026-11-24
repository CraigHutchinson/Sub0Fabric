#include "sub0fabric/pure/segment.hpp"

#include "sub0fabric/pure/crc32c.hpp"

#include "json_util.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace sub0fabric {

namespace {

constexpr std::string_view kMagic = "S0FSEG1\n";
constexpr std::string_view kFormat = "fabric-seg-1";
constexpr std::uint32_t kMaxHeaderBody = 64u << 10;

void putU32(std::string& out, std::uint32_t v)
{
    const char b[4] = {static_cast<char>(v & 0xFFu), static_cast<char>((v >> 8) & 0xFFu), static_cast<char>((v >> 16) & 0xFFu), static_cast<char>((v >> 24) & 0xFFu)};
    out.append(b, 4);
}

std::uint32_t getU32(const char* p) noexcept
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::uint32_t>(u[0]) | (static_cast<std::uint32_t>(u[1]) << 8) | (static_cast<std::uint32_t>(u[2]) << 16) | (static_cast<std::uint32_t>(u[3]) << 24);
}

bool allZero(std::string_view s) noexcept
{
    return std::ranges::all_of(s, [](char c) { return c == '\0'; });
}

}  // namespace

std::string_view framingName(Framing f) noexcept { return f == Framing::lines ? "lines" : "frames"; }

std::optional<Framing> parseFraming(std::string_view name) noexcept
{
    if (name == "lines") return Framing::lines;
    if (name == "frames") return Framing::frames;
    return std::nullopt;
}

void appendFrame(std::string& out, std::string_view payload)
{
    putU32(out, static_cast<std::uint32_t>(payload.size()));
    putU32(out, crc32c(std::string_view{out}.substr(out.size() - 4)));
    out.append(payload);
    putU32(out, crc32c(payload));
}

std::expected<void, std::string> appendRecord(std::string& out, Framing framing, const RecordCodec& codec, const Record& r)
{
    const std::size_t start = out.size();
    if (framing == Framing::lines) {
        codec.encode(r, out);
        if (std::string_view{out}.substr(start).contains('\n')) {
            out.resize(start);
            return std::unexpected("a record's text holds a newline: it would split a line (use the frames framing, or compact JSON)");
        }
        out.push_back('\n');
        return {};
    }
    // Reserve the 8 header bytes, encode in place, then fill in the length and its checksum: the payload is written once.
    out.append(8, '\0');
    codec.encode(r, out);
    const std::size_t length = out.size() - start - 8;
    if (length > kMaxFramePayload) {
        out.resize(start);
        return std::unexpected(std::format("a record of {} bytes is over the frame limit of {}", length, kMaxFramePayload));
    }
    std::string head;
    putU32(head, static_cast<std::uint32_t>(length));
    putU32(head, crc32c(std::string_view{head}));
    std::memcpy(out.data() + start, head.data(), 8);
    putU32(out, crc32c(std::string_view{out}.substr(start + 8, length)));
    return {};
}

std::string encodeHeader(const SegmentHeader& h)
{
    json::Writer w;
    w.beginObject().kv("format", kFormat).kv("shape", h.shape).kv("order", h.order);
    w.key("writer").beginObject().kv("namespace", h.writer.ns).kv("writer", h.writer.writer).endObject();
    w.kv("first", static_cast<std::int64_t>(h.first)).endObject();
    const std::string_view body = w.view();
    std::string out{kMagic};
    putU32(out, static_cast<std::uint32_t>(body.size()));
    out.append(body);
    putU32(out, crc32c(std::string_view{out}.substr(kMagic.size(), 4 + body.size())));
    return out;
}

HeaderParse parseHeader(std::string_view window, bool atEof)
{
    HeaderParse r;
    const auto short_ = [&]() { r.step = atEof ? Step::torn : Step::needMore; return r; };
    const auto bad = [&](std::string message) { r.step = Step::corrupt; r.error = std::move(message); return r; };
    if (atEof && allZero(window) && !window.empty()) { r.step = Step::torn; return r; }   // a file extended and not written
    const std::size_t have = std::min(window.size(), kMagic.size());
    if (window.substr(0, have) != kMagic.substr(0, have)) return bad("not a Fabric segment (the magic bytes differ)");
    if (window.size() < kMagic.size() + 4) return short_();
    const std::uint32_t bodyLength = getU32(window.data() + kMagic.size());
    if (bodyLength > kMaxHeaderBody) return bad(std::format("segment header claims a body of {} bytes", bodyLength));
    const std::size_t total = kMagic.size() + 4 + bodyLength + 4;
    if (window.size() < total) return short_();
    const std::uint32_t stored = getU32(window.data() + total - 4);
    if (crc32c(window.substr(kMagic.size(), 4 + bodyLength)) != stored) return bad("segment header checksum mismatch");
    const std::string_view body = window.substr(kMagic.size() + 4, bodyLength);
    bool formatOk = false;
    try {
        json::ParseSession session;
        auto doc = session.iterate(body);
        simdjson::ondemand::object obj = doc.get_object();
        for (auto field : obj) {
            const std::string_view key = field.unescaped_key();
            auto value = field.value();
            if (key == "format") { std::string_view f = value.get_string(); formatOk = f == kFormat; }
            else if (key == "shape") r.header.shape = json::toString(value);
            else if (key == "order") r.header.order = json::toString(value);
            else if (key == "first") r.header.first = value.get_uint64();
            else if (key == "writer") {
                for (auto wf : value.get_object()) {
                    const std::string_view wk = wf.unescaped_key();
                    auto wv = wf.value();
                    if (wk == "namespace") r.header.writer.ns = json::toString(wv);
                    else if (wk == "writer") r.header.writer.writer = json::toString(wv);
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        return bad(std::format("segment header is not valid JSON of the expected shape: {}", e.what()));
    }
    if (!formatOk) return bad("segment header names a format this build does not read");
    r.step = Step::ok;
    r.size = total;
    return r;
}

SegmentScanner::SegmentScanner(Framing framing, const RecordCodec& codec, std::uint64_t firstPosition, std::uint64_t startOffset)
    : framing_(framing), codec_(&codec), offset_(startOffset), validOffset_(startOffset), position_(firstPosition)
{
}

void SegmentScanner::feed(std::string_view bytes)
{
    if (head_ > 0 && (head_ >= buffer_.size() / 2 || head_ > (64u << 10))) {   // compact: the consumed prefix is never needed again
        buffer_.erase(0, head_);
        head_ = 0;
    }
    buffer_.append(bytes);
}

void SegmentScanner::reset(std::uint64_t firstPosition, std::uint64_t startOffset)
{
    buffer_.clear();
    head_ = 0;
    offset_ = validOffset_ = startOffset;
    position_ = firstPosition;
    finished_ = false;
    zeroStart_.reset();
    error_.clear();
}

Step SegmentScanner::corrupt(std::string message)
{
    error_ = std::move(message);
    return Step::corrupt;
}

Step SegmentScanner::next(Record* out) { return framing_ == Framing::lines ? nextLine(out) : nextFrame(out); }

Step SegmentScanner::accept(Record* target, bool /*decoded*/)
{
    const std::uint64_t expected = position_ + 1;
    if (codec_->carriesSeq()) {
        if (target->seq != expected) return Step::corrupt;   // the caller words the message (a torn last line is not an error)
    } else {
        target->seq = expected;
    }
    return Step::ok;
}

Step SegmentScanner::nextLine(Record* out)
{
    const std::string_view win = window();
    const std::size_t nl = win.find('\n');
    if (nl == std::string_view::npos) {
        if (win.empty()) return finished_ ? Step::end : Step::needMore;
        if (!finished_) return Step::needMore;
        error_ = "partial final line";
        return Step::torn;
    }
    Record* target = out != nullptr ? out : &scratch_;
    const std::size_t consumed = nl + 1;
    bool good = codec_->decode(win.substr(0, nl), *target).has_value();
    if (good) good = accept(target, true) == Step::ok;
    if (!good) {
        if (consumed == win.size()) {   // a bad last line is a torn tail, as journal-1 treats it; it is only known to be last once the end is known
            if (!finished_) return Step::needMore;
            error_ = "bad final line";
            return Step::torn;
        }
        return corrupt(std::format("corrupt record before the end of the segment: line starting at byte {}", offset_));
    }
    consume(consumed);
    ++position_;
    validOffset_ = offset_;
    return Step::ok;
}

Step SegmentScanner::inZeroRun()
{
    const std::string_view win = window();
    if (!allZero(win)) return corrupt(std::format("data after a zeroed region that began at byte {}", *zeroStart_));
    consume(win.size());   // zeros need not be kept: if a non-zero byte follows later, the region start is all the error needs
    if (finished_) {
        error_ = "zeroed tail";
        return Step::torn;
    }
    return Step::needMore;
}

Step SegmentScanner::nextFrame(Record* out)
{
    if (zeroStart_) return inZeroRun();
    const std::string_view win = window();
    if (win.empty()) return finished_ ? Step::end : Step::needMore;
    if (win.size() < 8) {
        if (!finished_) return Step::needMore;
        error_ = "partial frame header";
        return Step::torn;
    }
    const std::uint32_t length = getU32(win.data());
    if (crc32c(win.substr(0, 4)) != getU32(win.data() + 4)) {
        // A crash leaves a clean prefix of what was written, so a complete header that fails its own checksum is damage, unless it is the zeroed tail
        // some file systems leave after a crash.
        if (allZero(win)) {
            zeroStart_ = offset_;
            return inZeroRun();
        }
        return corrupt(std::format("frame header checksum mismatch at byte {}", offset_));
    }
    if (length > kMaxFramePayload) return corrupt(std::format("frame at byte {} claims {} bytes", offset_, length));
    const std::size_t total = 8 + static_cast<std::size_t>(length) + 4;
    if (win.size() < total) {
        if (!finished_) return Step::needMore;
        error_ = "partial frame";
        return Step::torn;
    }
    const std::string_view payload = win.substr(8, length);
    if (crc32c(payload) != getU32(win.data() + 8 + length)) {
        if (win.size() == total) {   // the last thing in the segment: torn once the end is known
            if (!finished_) return Step::needMore;
            error_ = "bad final frame";
            return Step::torn;
        }
        return corrupt(std::format("frame checksum mismatch before the end of the segment: frame at byte {}", offset_));
    }
    if (out != nullptr) {
        if (auto decoded = codec_->decode(payload, *out); !decoded)
            return corrupt(std::format("frame at byte {} passes its checksum but is not a {} record: {}", offset_, codec_->shape(), decoded.error()));
        if (accept(out, true) != Step::ok) return corrupt(std::format("frame at byte {} holds seq {} where position {} is expected", offset_, out->seq, position_ + 1));
    }
    consume(total);
    ++position_;
    validOffset_ = offset_;
    return Step::ok;
}

namespace {

/// Drive a scanner over a whole image.
std::expected<ImageScan, std::string> drain(SegmentScanner& scanner, std::string_view bytes, ImageScan scan, const RecordCodec& codec)
{
    scanner.feed(bytes);
    scanner.finish();
    for (;;) {
        Record r;
        const Step s = scanner.next(&r);
        if (s == Step::ok) {
            if (!codec.carriesWriter()) r.writer = scan.header.writer;
            scan.records.push_back(std::move(r));
            continue;
        }
        if (s == Step::corrupt) return std::unexpected(scanner.error());
        scan.tornTail = s == Step::torn;
        break;
    }
    scan.validBytes = scanner.validOffset();
    return scan;
}

}  // namespace

std::expected<ImageScan, std::string> scanFrameImage(std::string_view image)
{
    const HeaderParse hp = parseHeader(image, true);
    ImageScan scan;
    if (image.empty()) return scan;   // a file with nothing in it: a clean, empty segment
    if (hp.step == Step::torn) {
        scan.tornTail = true;
        return scan;
    }
    if (hp.step != Step::ok) return std::unexpected(hp.error);
    const RecordCodec* codec = findCodec(hp.header.shape);
    if (codec == nullptr) return std::unexpected(std::format("segment names the record shape '{}', which this build does not know", hp.header.shape));
    scan.header = hp.header;
    SegmentScanner scanner(Framing::frames, *codec, hp.header.first, hp.size);
    return drain(scanner, image.substr(hp.size), std::move(scan), *codec);
}

std::expected<ImageScan, std::string> scanLineImage(std::string_view image, const RecordCodec& codec, const SegmentHeader& header)
{
    ImageScan scan;
    scan.header = header;
    SegmentScanner scanner(Framing::lines, codec, header.first, 0);
    return drain(scanner, image, std::move(scan), codec);
}

}  // namespace sub0fabric
