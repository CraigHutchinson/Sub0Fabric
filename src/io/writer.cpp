#include "sub0fabric/io/store.hpp"

#include "segment_file.hpp"

#include "sub0fabric/pure/layout.hpp"

#include <format>

namespace sub0fabric::io {

namespace fs = std::filesystem;

namespace {

fs::path streamDir(const fs::path& root, const WriterId& writer, Layout layout)
{
    return layout == Layout::shared ? root : root / encodeName(writer.ns) / encodeName(writer.writer);
}

std::string perr(const sub0platform::PlatformError& e) { return e.message; }

}  // namespace

StoreWriter::~StoreWriter()
{
    if (file_.isOpen() && (dirty_ || !buffer_.empty())) {
        (void)flushBuffer();
        (void)file_.sync();
    }
}

std::expected<StoreWriter, std::string> StoreWriter::open(const fs::path& root, WriterId writer, WriterOptions options)
{
    const RecordCodec* codec = findCodec(options.shape);
    if (codec == nullptr) return std::unexpected(std::format("unknown record shape '{}'", options.shape));
    if (options.order.empty()) options.order = std::string{codec->defaultOrder()};
    if (findOrderPolicy(options.order) == nullptr) return std::unexpected(std::format("unknown order policy '{}'", options.order));

    StoreWriter w;
    w.dir_ = streamDir(root, writer, options.layout);
    w.writer_ = std::move(writer);
    w.options_ = std::move(options);
    w.codec_ = codec;
    w.order_ = w.options_.order;
    std::error_code ec;
    fs::create_directories(w.dir_, ec);
    if (ec) return std::unexpected("cannot create " + w.dir_.string() + ": " + ec.message());

    if (w.options_.lock) {
        auto lock = sub0platform::FileLock::tryExclusive(w.dir_ / ".lock");
        if (!lock) return std::unexpected(perr(lock.error()));
        if (!*lock) return std::unexpected("another writer holds " + w.dir_.string());
        w.lock_ = std::move(**lock);
    }

    // The active segment is the last one; a clean directory starts the first.
    if (w.options_.framing == Framing::lines) {
        const fs::path path = w.dir_ / std::string{kJournalFileName};
        w.segmentNumber_ = 1;
        if (fs::exists(path, ec)) {
            auto scan = detail::scanFile(path, Framing::lines, *codec, 0, 0, nullptr, 64u << 10);
            if (!scan) return std::unexpected(scan.error());
            if (!scan->corrupt.empty()) return std::unexpected(std::format("{} in {}", scan->corrupt, path.string()));
            w.nextSeq_ = scan->position + 1;
            w.segmentBytes_ = scan->validOffset;
            w.segmentRecords_ = scan->records;
            w.recoveredBytes_ = scan->fileSize - scan->validOffset;
        }
        auto file = sub0platform::AppendFile::open(path);
        if (!file) return std::unexpected("cannot open " + path.string() + ": " + perr(file.error()));
        w.file_ = std::move(*file);
        if (w.recoveredBytes_ > 0)
            if (auto cut = w.file_.truncate(w.segmentBytes_); !cut) return std::unexpected("cannot truncate torn tail of " + path.string() + ": " + perr(cut.error()));
        return w;
    }

    const auto segments = detail::listSegments(w.dir_);
    if (segments.empty()) {
        if (auto begun = w.beginSegment(1, 0); !begun) return std::unexpected(begun.error());
        return w;
    }
    const auto& [number, path] = segments.back();
    auto head = detail::readHeader(path);
    if (!head) return std::unexpected(head.error());
    if (head->parse.step == Step::corrupt) return std::unexpected(std::format("{}: {}", path.string(), head->parse.error));
    std::uint64_t first = 0;
    if (head->parse.step == Step::torn) {
        // A crash during the header write: the file holds no records. Its first position is where the segment before it ended.
        if (segments.size() > 1) {
            const auto& [prevNumber, prevPath] = segments[segments.size() - 2];
            (void)prevNumber;
            auto prevHead = detail::readHeader(prevPath);
            if (!prevHead) return std::unexpected(prevHead.error());
            if (prevHead->parse.step != Step::ok) return std::unexpected(std::format("{}: segment before a torn one is unreadable", prevPath.string()));
            auto prev = detail::scanFile(prevPath, Framing::frames, *codec, prevHead->parse.header.first, prevHead->parse.size, nullptr, 64u << 10);
            if (!prev) return std::unexpected(prev.error());
            first = prev->position;
        }
        auto file = sub0platform::AppendFile::open(path);
        if (!file) return std::unexpected("cannot open " + path.string() + ": " + perr(file.error()));
        if (auto cut = file->truncate(0); !cut) return std::unexpected("cannot truncate " + path.string() + ": " + perr(cut.error()));
        file = {};
        w.recoveredBytes_ = head->fileSize;
        if (auto begun = w.beginSegment(number, first); !begun) return std::unexpected(begun.error());
        return w;
    }
    const SegmentHeader& h = head->parse.header;
    if (h.shape != w.options_.shape) return std::unexpected(std::format("{} holds shape '{}', not '{}'", path.string(), h.shape, w.options_.shape));
    if (h.writer != w.writer_) return std::unexpected(std::format("{} belongs to writer '{}/{}', not '{}/{}'", path.string(), h.writer.ns, h.writer.writer, w.writer_.ns, w.writer_.writer));
    w.order_ = h.order;   // the stream's own order policy stands
    auto scan = detail::scanFile(path, Framing::frames, *codec, h.first, head->parse.size, nullptr, 64u << 10);
    if (!scan) return std::unexpected(scan.error());
    if (!scan->corrupt.empty()) return std::unexpected(std::format("{} in {}", scan->corrupt, path.string()));
    w.segmentNumber_ = number;
    w.nextSeq_ = scan->position + 1;
    w.segmentBytes_ = scan->validOffset;
    w.segmentRecords_ = scan->records;
    w.recoveredBytes_ = scan->fileSize - scan->validOffset;
    auto file = sub0platform::AppendFile::open(path);
    if (!file) return std::unexpected("cannot open " + path.string() + ": " + perr(file.error()));
    w.file_ = std::move(*file);
    if (w.recoveredBytes_ > 0)
        if (auto cut = w.file_.truncate(w.segmentBytes_); !cut) return std::unexpected("cannot truncate torn tail of " + path.string() + ": " + perr(cut.error()));
    return w;
}

std::expected<void, std::string> StoreWriter::beginSegment(std::uint64_t number, std::uint64_t first)
{
    const fs::path path = dir_ / segmentFileName(number);
    auto file = sub0platform::AppendFile::open(path);
    if (!file) return std::unexpected("cannot open " + path.string() + ": " + perr(file.error()));
    SegmentHeader h;
    h.shape = options_.shape;
    h.order = order_;
    h.writer = writer_;
    h.first = first;
    const std::string bytes = encodeHeader(h);
    if (auto wrote = file->append(bytes); !wrote) return std::unexpected("cannot write " + path.string() + ": " + perr(wrote.error()));
    file_ = std::move(*file);
    segmentNumber_ = number;
    segmentBytes_ = bytes.size();
    segmentRecords_ = 0;
    nextSeq_ = first + 1;
    dirty_ = true;
    return {};
}

std::expected<void, std::string> StoreWriter::flushBuffer()
{
    if (buffer_.empty()) return {};
    if (auto wrote = file_.append(buffer_); !wrote) return std::unexpected("write failed: " + perr(wrote.error()));
    segmentBytes_ += buffer_.size();
    buffer_.clear();
    dirty_ = true;
    return {};
}

std::expected<void, std::string> StoreWriter::afterAppend(bool batch)
{
    using Mode = Durability::Mode;
    const auto now = std::chrono::steady_clock::now();
    const Mode mode = options_.durability.mode;
    const bool due = batch || mode == Mode::perRecord || (mode == Mode::interval && now - lastSync_ >= options_.durability.interval);
    if (!due) return {};
    return sync();
}

std::expected<std::uint64_t, std::string> StoreWriter::append(Record& r)
{
    return appendBatch(std::span<Record>{&r, 1});
}

std::expected<std::uint64_t, std::string> StoreWriter::appendBatch(std::span<Record> records)
{
    if (!file_.isOpen()) return std::unexpected("store writer is not open");
    if (records.empty()) return nextSeq_ - 1;
    for (Record& r : records) {
        if (codec_->carriesWriter()) {
            if (r.writer == WriterId{}) r.writer = writer_;
            else if (r.writer != writer_) return std::unexpected(std::format("record names writer '{}/{}' but this stream belongs to '{}/{}'", r.writer.ns, r.writer.writer, writer_.ns, writer_.writer));
        }
        r.seq = nextSeq_;
        const std::size_t before = buffer_.size();
        if (auto encoded = appendRecord(buffer_, options_.framing, *codec_, r); !encoded) {
            buffer_.resize(before);
            return std::unexpected(encoded.error());
        }
        ++nextSeq_;
        // Roll before the segment passes its limit: the record just encoded begins the next segment. A segment always holds at least one record.
        if (options_.framing == Framing::frames && segmentBytes_ + buffer_.size() > options_.maxSegmentBytes && segmentRecords_ > 0) {
            const std::string pending = buffer_.substr(before);
            buffer_.resize(before);
            if (auto flushed = flushBuffer(); !flushed) return std::unexpected(flushed.error());
            if (auto synced = file_.sync(); !synced) return std::unexpected("flush failed: " + perr(synced.error()));
            if (auto begun = beginSegment(segmentNumber_ + 1, r.seq - 1); !begun) return std::unexpected(begun.error());
            nextSeq_ = r.seq + 1;
            segmentRecords_ = 0;
            buffer_ = pending;
        }
        ++segmentRecords_;
    }
    if (auto flushed = flushBuffer(); !flushed) return std::unexpected(flushed.error());
    if (auto after = afterAppend(records.size() > 1); !after) return std::unexpected(after.error());
    return nextSeq_ - 1;
}

std::expected<void, std::string> StoreWriter::sync()
{
    if (!file_.isOpen()) return std::unexpected("store writer is not open");
    if (auto flushed = flushBuffer(); !flushed) return flushed;
    if (auto synced = file_.sync(); !synced) return std::unexpected("flush failed: " + perr(synced.error()));
    dirty_ = false;
    lastSync_ = std::chrono::steady_clock::now();
    return {};
}

}  // namespace sub0fabric::io
