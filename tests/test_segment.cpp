// The segment byte format (segment.hpp): CRC-32C, frames, header, the scanner's torn-tail and corruption rules for both framings, chunked feeding, and the
// memory bound of a scan. The truncation fuzz is Sub0Squad's journal test, generalised to every framing and shape.
#include "helpers.hpp"
#include "sub0fabric/pure/crc32c.hpp"
#include "sub0fabric/pure/segment.hpp"
#include "sub0platform/testing/check.hpp"

using namespace sub0fabric;
using namespace fabrictest;

namespace {

SegmentHeader headerFor(std::string_view shape, std::uint64_t first = 0)
{
    SegmentHeader h;
    h.shape = std::string{shape};
    h.order = std::string{findCodec(shape)->defaultOrder()};
    h.writer = {"proj", "w-000001"};
    h.first = first;
    return h;
}

std::vector<Record> sampleCanvas(int n)
{
    std::vector<Record> v;
    for (int i = 1; i <= n; ++i) v.push_back(canvasRecord("proj", "w-000001", static_cast<std::uint64_t>(i), "item.put@1", R"({"i":)" + std::to_string(i) + "}"));
    return v;
}

/// Scan `bytes` in pieces of `piece` bytes (0: all at once) and return the records and the final step.
struct Walk
{
    std::vector<Record> records;
    Step last = Step::ok;
    std::uint64_t validOffset = 0;
    std::size_t peakBuffered = 0;
    std::string error;
};
Walk walk(Framing f, const RecordCodec& codec, std::string_view bytes, std::size_t piece, std::uint64_t first = 0, std::uint64_t startOffset = 0)
{
    Walk w;
    SegmentScanner s(f, codec, first, startOffset);
    std::size_t fed = 0;
    for (;;) {
        Record r;
        const Step st = s.next(&r);
        w.peakBuffered = std::max(w.peakBuffered, s.buffered());
        if (st == Step::ok) { w.records.push_back(std::move(r)); continue; }
        if (st == Step::needMore) {
            if (fed >= bytes.size()) { s.finish(); continue; }
            const std::size_t n = piece == 0 ? bytes.size() - fed : std::min(piece, bytes.size() - fed);
            s.feed(bytes.substr(fed, n));
            fed += n;
            continue;
        }
        w.last = st;
        w.validOffset = s.validOffset();
        w.error = s.error();
        return w;
    }
}

}  // namespace

int main()
{
    // CRC-32C: the standard check value, and continuation.
    CHECK_EQ(crc32c("123456789"), 0xE3069283u);
    CHECK_EQ(crc32c(""), 0u);
    CHECK_EQ(crc32c("6789", crc32c("12345")), crc32c("123456789"));
    {
        std::string big(1000, 'x');
        for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);
        std::uint32_t byBytes = 0;
        for (char c : big) byBytes = crc32c(std::string_view{&c, 1}, byBytes);
        CHECK_EQ(crc32c(big), byBytes);   // slice-by-8 equals byte-at-a-time
    }

    const RecordCodec& canvas = *findCodec("canvas-event-1");
    const RecordCodec& journal = *findCodec("journal-1");

    // Header: round trip, every truncation is torn, a flipped byte is corrupt, the magic is checked.
    {
        SegmentHeader h = headerFor("canvas-event-1", 41);
        h.writer = {"a/b \"c\"", "w\n1"};
        const std::string bytes = encodeHeader(h);
        auto p = parseHeader(bytes, true);
        CHECK(p.step == Step::ok && p.header == h && p.size == bytes.size());
        for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
            CHECK(parseHeader(std::string_view{bytes}.substr(0, cut), false).step == Step::needMore);
            const Step atEof = parseHeader(std::string_view{bytes}.substr(0, cut), true).step;
            CHECK(atEof == Step::torn);
        }
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            std::string bad = bytes;
            bad[i] = static_cast<char>(bad[i] ^ 0x40);
            const Step s = parseHeader(bad, true).step;
            CHECK(s == Step::corrupt || s == Step::torn);   // a flipped length byte can make the header look longer than the bytes: torn, never ok
            CHECK(s != Step::ok);
        }
        CHECK(parseHeader("hello world, not a segment", true).step == Step::corrupt);
        CHECK(parseHeader(std::string(30, '\0'), true).step == Step::torn);   // a zero-extended file
    }

    // Frames: a whole image reads back; the record's position is its seq.
    {
        const auto recs = sampleCanvas(5);
        const std::string image = frameImage(headerFor("canvas-event-1"), recs);
        auto scan = scanFrameImage(image);
        CHECK(scan.has_value());
        if (scan) {
            CHECK_EQ(scan->records.size(), 5u);
            CHECK(!scan->tornTail);
            CHECK_EQ(scan->validBytes, image.size());
            for (std::size_t i = 0; i < 5; ++i) {
                CHECK_EQ(scan->records[i].seq, i + 1);
                CHECK_EQ(scan->records[i].lamport, i + 1);
                CHECK(scan->records[i].writer == recs[i].writer);
            }
        }
        // A rolled segment continues the count.
        auto rolled = scanFrameImage(frameImage(headerFor("canvas-event-1", 100), sampleCanvas(2)));
        CHECK(rolled.has_value() && rolled->records.size() == 2 && rolled->records[0].seq == 101 && rolled->records[1].seq == 102);
    }

    // Truncation fuzz, frames: cut at every byte; the scan never fails, returns a prefix, and says torn exactly when bytes were left over.
    {
        const std::string image = frameImage(headerFor("canvas-event-1"), sampleCanvas(4));
        std::vector<std::uint64_t> boundaries;   // offsets just after the header and each frame
        {
            auto full = scanFrameImage(image);
            CHECK(full.has_value());
            SegmentScanner s(Framing::frames, canvas, 0, parseHeader(image, true).size);
            s.feed(std::string_view{image}.substr(parseHeader(image, true).size));
            s.finish();
            boundaries.push_back(s.validOffset());
            Record r;
            while (s.next(&r) == Step::ok) boundaries.push_back(s.validOffset());
        }
        for (std::size_t cut = 0; cut <= image.size(); ++cut) {
            auto scan = scanFrameImage(std::string_view{image}.substr(0, cut));
            CHECK(scan.has_value());
            if (!scan) continue;
            CHECK(scan->records.size() <= 4u);
            for (std::size_t i = 0; i < scan->records.size(); ++i) CHECK_EQ(scan->records[i].seq, i + 1);
            const bool atBoundary = cut == 0 || std::ranges::find(boundaries, static_cast<std::uint64_t>(cut)) != boundaries.end();
            CHECK_EQ(scan->tornTail, !atBoundary);
            if (!scan->tornTail) CHECK_EQ(scan->validBytes, cut);
            else CHECK(scan->validBytes <= cut);
            // Cut the torn bytes: the rest scans clean (what a writer does on open).
            if (scan->tornTail && scan->validBytes > 0) {
                auto again = scanFrameImage(std::string_view{image}.substr(0, scan->validBytes));
                CHECK(again.has_value() && !again->tornTail && again->records.size() == scan->records.size());
            }
        }
    }

    // Truncation fuzz, lines, both shapes: journal-1's rules.
    for (const RecordCodec* codec : {&journal, &canvas}) {
        std::vector<Record> recs;
        if (codec == &journal) { recs = {journalRecord("a", R"({"x":1})"), journalRecord("b", R"({"s":"q\"uote\n"})"), journalRecord("c", "{}")}; }
        else recs = sampleCanvas(3);
        const std::string image = lineImage(*codec, recs);
        SegmentHeader h = headerFor(codec->shape());
        for (std::size_t cut = 0; cut <= image.size(); ++cut) {
            auto scan = scanLineImage(std::string_view{image}.substr(0, cut), *codec, h);
            CHECK(scan.has_value());
            if (!scan) continue;
            CHECK(scan->records.size() <= 3u);
            for (std::size_t i = 0; i < scan->records.size(); ++i) CHECK_EQ(scan->records[i].seq, i + 1);
            const bool atLineEnd = cut == 0 || image[cut - 1] == '\n';
            CHECK_EQ(scan->tornTail, !atLineEnd);
            if (scan->tornTail) CHECK(scan->validBytes < cut);
        }
    }

    // Corruption before the end is reported, never skipped; a bad last record is torn. Frames and lines.
    {
        const std::string image = frameImage(headerFor("canvas-event-1"), sampleCanvas(4));
        const std::size_t headerSize = parseHeader(image, true).size;
        std::string bad = image;
        bad[headerSize + 10] = static_cast<char>(bad[headerSize + 10] ^ 0x01);   // inside the first frame's payload
        CHECK(!scanFrameImage(bad).has_value());
        std::string badLen = image;
        badLen[headerSize] = static_cast<char>(badLen[headerSize] ^ 0x01);       // the first frame's length: caught by its own checksum
        CHECK(!scanFrameImage(badLen).has_value());
        std::string badLast = image;
        badLast[badLast.size() - 6] = static_cast<char>(badLast[badLast.size() - 6] ^ 0x01);   // the last frame's payload: torn
        auto lastScan = scanFrameImage(badLast);
        CHECK(lastScan.has_value() && lastScan->tornTail && lastScan->records.size() == 3);

        const std::string lines = lineImage(journal, {journalRecord("a", "{}"), journalRecord("b", "{}"), journalRecord("c", "{}")});
        SegmentHeader jh = headerFor("journal-1");
        std::string badLine = lines;
        badLine.at(lines.find('\n') / 2) = '#';
        CHECK(!scanLineImage(badLine, journal, jh).has_value());
        std::string badLastLine = lines;
        badLastLine.at(lines.rfind("\"type\"") + 2) = '#';   // the last line's key becomes "t#pe": a missing member (the parser does not validate an unconsumed value)
        auto bl = scanLineImage(badLastLine, journal, jh);
        CHECK(bl.has_value() && bl->tornTail && bl->records.size() == 2);
        // A line with the wrong seq is corruption in the middle and torn at the end (journal-1's `seq == expected` rule).
        std::string skipped = lines;
        const auto at = skipped.find("\"seq\":2");
        skipped.at(at + 6) = '9';
        CHECK(!scanLineImage(skipped, journal, jh).has_value());
    }

    // A zero tail (a file extended and not written) is torn; data after zeros is corruption.
    {
        const std::string image = frameImage(headerFor("canvas-event-1"), sampleCanvas(2));
        for (std::size_t zeros : {1u, 7u, 8u, 12u, 100u, 5000u}) {
            auto scan = scanFrameImage(image + std::string(zeros, '\0'));
            CHECK(scan.has_value() && scan->tornTail && scan->records.size() == 2 && scan->validBytes == image.size());
        }
        CHECK(!scanFrameImage(image + std::string(20, '\0') + "x").has_value());
    }

    // Oversized lengths are corruption, not an allocation.
    {
        std::string frame;
        const std::string_view big = "x";
        appendFrame(frame, big);
        frame[3] = static_cast<char>(0x7F);   // a length of about 2 GB, with a checksum that now fails
        std::string image = encodeHeader(headerFor("canvas-event-1")) + frame;
        CHECK(!scanFrameImage(image).has_value());
    }

    // Chunked feeding gives the same records at every piece size, and the buffer stays at the piece plus one frame.
    {
        const auto recs = sampleCanvas(200);
        const std::string image = frameImage(headerFor("canvas-event-1"), recs);
        const std::size_t hs = parseHeader(image, true).size;
        const std::string_view body = std::string_view{image}.substr(hs);
        const Walk whole = walk(Framing::frames, canvas, body, 0, 0, hs);
        CHECK_EQ(whole.records.size(), 200u);
        CHECK(whole.last == Step::end);
        for (std::size_t piece : {1u, 2u, 3u, 7u, 13u, 64u, 1000u}) {
            const Walk w = walk(Framing::frames, canvas, body, piece, 0, hs);
            CHECK(w.last == Step::end);
            CHECK(w.records == whole.records);
            CHECK(w.peakBuffered <= piece + 400);   // one frame is about 130 bytes; the buffer never holds the segment (the image is ~26 KB)
        }
        const std::string lines = lineImage(canvas, recs);
        const Walk lw = walk(Framing::lines, canvas, lines, 5);
        CHECK(lw.last == Step::end && lw.records.size() == 200u);
        CHECK(lw.peakBuffered < 400);
    }

    // Seeking: a scanner reset to a recorded offset yields the rest from there (what the sparse index relies on).
    {
        const auto recs = sampleCanvas(10);
        const std::string image = frameImage(headerFor("canvas-event-1"), recs);
        const std::size_t hs = parseHeader(image, true).size;
        SegmentScanner s(Framing::frames, canvas, 0, hs);
        s.feed(std::string_view{image}.substr(hs));
        s.finish();
        std::vector<std::uint64_t> starts;
        for (;;) {
            starts.push_back(s.offset());
            if (s.next(nullptr) != Step::ok) break;   // a null out checks the frame without decoding it
        }
        CHECK_EQ(s.position(), 10u);
        CHECK_EQ(starts.size(), 11u);
        s.reset(6, starts[6]);
        s.feed(std::string_view{image}.substr(starts[6]));
        s.finish();
        Record r;
        CHECK(s.next(&r) == Step::ok && r.seq == 7 && r.lamport == 7);
    }

    // A lines record whose text holds a newline is refused rather than split.
    {
        std::string out;
        CHECK(!appendRecord(out, Framing::lines, journal, journalRecord("a", "{\n}")).has_value());
        CHECK(out.empty());
        CHECK(appendRecord(out, Framing::frames, journal, journalRecord("a", "{\n}")).has_value());
    }

    CHECK(parseFraming("lines") == Framing::lines && parseFraming("frames") == Framing::frames && !parseFraming("x"));
    return finish();
}
