#pragma once
/// @file segment.hpp
/// The segment byte format: how records become bytes, how a crash is survived, and how a reader walks bytes it is handed in pieces. Two framings are
/// proved (review D2: `segment`), over any record shape (codec.hpp):
///
///  * `lines`: one record per `\n`-terminated line, no checksum. This is exactly Sub0Squad's `journal-1` file, so an existing journal opens unchanged.
///    A line that is not a record, or whose `seq` is not the position it sits at, is a torn tail when it is the last line and corruption otherwise.
///  * `frames`: a header, then frames `[u32 len][u32 crc32c(len bytes)][payload][u32 crc32c(payload)]`, little-endian. See DESIGN.md "Segment format".
///
/// Everything here is pure: the same bytes in, the same result out. Reading the file in chunks is the io layer's job; the scanner takes the chunks.

#include "sub0fabric/pure/codec.hpp"
#include "sub0fabric/pure/record.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sub0fabric {

enum class Framing
{
    lines,    ///< NDJSON, one record per line: the journal-1 file
    frames,   ///< length-prefixed, checksummed frames after a header
};
[[nodiscard]] std::string_view framingName(Framing f) noexcept;
[[nodiscard]] std::optional<Framing> parseFraming(std::string_view name) noexcept;

/// Bytes a frame adds to its payload: length, length checksum, payload checksum.
inline constexpr std::size_t kFrameOverhead = 12;
/// The largest frame payload a reader believes. A length beyond it is corruption, so a damaged length cannot make a reader allocate without bound.
inline constexpr std::uint32_t kMaxFramePayload = 64u << 20;

/// What the next step of a scan found.
enum class Step
{
    ok,         ///< a record (or the header) was read
    needMore,   ///< the bytes fed so far end inside a frame or line: feed more, or call finish()
    end,        ///< a clean end: the bytes ended exactly after a record
    torn,       ///< the bytes end in a record a crash left unfinished: everything before it is good
    corrupt,    ///< damage before the end: nothing after it can be trusted; error() names it
};

/// Append `r`, encoded with `codec`, to `out` in `framing`. The error: a `lines` record whose text holds a newline (it would split the record).
[[nodiscard]] std::expected<void, std::string> appendRecord(std::string& out, Framing framing, const RecordCodec& codec, const Record& r);
/// Append one frame holding `payload`.
void appendFrame(std::string& out, std::string_view payload);

/// The facts a `frames` segment states about itself, in its header (review D4: the order rule is data in the stream, not a constant of the library).
struct SegmentHeader
{
    std::string shape;       ///< the record shape's name (codec.hpp)
    std::string order;       ///< the order policy name (order.hpp)
    WriterId writer;         ///< whose stream this is
    std::uint64_t first = 0; ///< the 0-based position of this segment's first record in the writer's stream (a rolled segment continues the count)
    bool operator==(const SegmentHeader&) const = default;
};

/// The `frames` header bytes: magic `S0FSEG1\n`, `[u32 body length][body: a JSON object][u32 crc32c(length bytes + body)]`.
[[nodiscard]] std::string encodeHeader(const SegmentHeader& h);

/// The result of reading a header from the bytes at the start of a segment file.
struct HeaderParse
{
    Step step = Step::needMore;   ///< ok, needMore, torn (the file ends inside the header) or corrupt (not a segment, or a bad checksum)
    SegmentHeader header;         ///< valid when step is ok
    std::size_t size = 0;         ///< bytes the header occupies, when step is ok
    std::string error;            ///< when step is corrupt
};
/// Read a header from `window`. `atEof`: no more bytes will follow, so a short window is torn rather than incomplete.
[[nodiscard]] HeaderParse parseHeader(std::string_view window, bool atEof);

/// Walks the records of one segment from bytes handed over in pieces: the memory held is the piece in hand and the largest frame, never the segment.
/// Not thread-safe. The rules (journal-1's, kept):
///  * a record that is incomplete or bad and is the last thing in the segment is a torn tail (`torn`): a crash left it, `validOffset()` is where to cut;
///  * damage anywhere before the end is `corrupt`: it is reported, never skipped;
///  * a `frames` tail of zero bytes (a file extended and not written, which some file systems leave after a crash) is torn.
class SegmentScanner
{
public:
    /// `firstPosition`: the 0-based position of the first record fed. `startOffset`: the file offset of the first byte fed (after the header for `frames`),
    /// for error messages and `validOffset()`.
    SegmentScanner(Framing framing, const RecordCodec& codec, std::uint64_t firstPosition, std::uint64_t startOffset);

    /// Append bytes. The views `next` hands out are invalidated.
    void feed(std::string_view bytes);
    /// No more bytes will follow: an incomplete last record is torn, and the end is `end`.
    void finish() noexcept { finished_ = true; }
    /// Read the next record into `out`; with a null `out` the record is only checked (a `frames` payload is checked by its checksum and not decoded; a `lines`
    /// line must be decoded to be checked, as journal-1's `seq` check requires). Sets `out->seq` to the record's position + 1.
    [[nodiscard]] Step next(Record* out);
    /// Start again at another position of the same segment (an index seek): drops the bytes held. Offsets and positions continue from the arguments.
    void reset(std::uint64_t firstPosition, std::uint64_t startOffset);

    /// The file offset just after the last good record (the offset to cut the file at after a torn tail).
    [[nodiscard]] std::uint64_t validOffset() const noexcept { return validOffset_; }
    /// The 0-based position of the next record; after the end, the number of records the stream holds up to here.
    [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
    /// The file offset where the next record starts (before `next`), for a sparse index.
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }
    /// Bytes held in the buffer (tests hold the memory bound to this).
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - head_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    [[nodiscard]] Step nextLine(Record* out);
    [[nodiscard]] Step nextFrame(Record* out);
    [[nodiscard]] Step inZeroRun();
    [[nodiscard]] Step accept(Record* target, bool decoded);
    void consume(std::size_t n) noexcept { head_ += n; offset_ += n; }
    [[nodiscard]] std::string_view window() const noexcept { return std::string_view{buffer_}.substr(head_); }
    Step corrupt(std::string message);

    Framing framing_;
    const RecordCodec* codec_;
    std::string buffer_;
    std::size_t head_ = 0;
    std::uint64_t offset_;
    std::uint64_t validOffset_;
    std::uint64_t position_;
    bool finished_ = false;
    std::optional<std::uint64_t> zeroStart_;   ///< set while skipping a run of zero bytes that is torn only if it reaches the end
    std::string error_;
    Record scratch_;
};

/// A whole segment image read at once: tests, fixtures, small files.
struct ImageScan
{
    SegmentHeader header;                ///< `frames`: from the file; `lines`: as passed to scanImage
    std::vector<Record> records;         ///< the good records, in order, each with `seq` set (and `writer` stamped from the header when the shape has none)
    std::uint64_t validBytes = 0;        ///< the offset after the last good record: cut a torn file here
    bool tornTail = false;
};
/// Scan a `frames` image. A file that ends inside its header is a torn tail with no records. The error: not a segment, an unknown shape, or corruption
/// before the end.
[[nodiscard]] std::expected<ImageScan, std::string> scanFrameImage(std::string_view image);
/// Scan a `lines` image of `codec`'s shape; `header` says whose stream it is and the position of its first record.
[[nodiscard]] std::expected<ImageScan, std::string> scanLineImage(std::string_view image, const RecordCodec& codec, const SegmentHeader& header);

}  // namespace sub0fabric
