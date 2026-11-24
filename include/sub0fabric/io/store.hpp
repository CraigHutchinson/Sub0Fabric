#pragma once
/// @file store.hpp
/// The directory store (review D2 `store`, D5): one writer owns each segment, so appending takes no lock between writers; a reader merges every writer's
/// segments into one deterministic order, streaming, and resumes from a per-writer cursor.
///
/// Layout (pure/layout.hpp): `<root>/<ns>/<writer>/seg-000001.s0f` (`Layout::perWriter`, the default), or `<root>/seg-000001.s0f` for one stream shared by
/// one process at a time (`Layout::shared`). A `lines` stream is the single file `journal-000001.ndjson` in the same place.

#include "sub0fabric/pure/cursor.hpp"
#include "sub0fabric/pure/index.hpp"
#include "sub0fabric/pure/order.hpp"
#include "sub0fabric/pure/record.hpp"
#include "sub0fabric/pure/segment.hpp"
#include "sub0platform/file.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sub0fabric::io {

/// Where a writer's segments live.
enum class Layout
{
    perWriter,   ///< a directory per writer under the root (review D5): any number of writers, no lock between them
    shared,      ///< the root itself is one stream: one process at a time, whatever the writer id (Sub0Squad's journal directory)
};

/// When appended records are flushed to the device (review section 5: durability is a policy).
struct Durability
{
    enum class Mode
    {
        perRecord,   ///< every append is flushed before it returns
        perBatch,    ///< `appendBatch` and `sync()` flush; a single `append` does not (the default)
        interval,    ///< an append flushes when `interval` has passed since the last flush; `appendBatch` and `sync()` always do
    };
    Mode mode = Mode::perBatch;
    std::chrono::milliseconds interval{1000};
};

struct WriterOptions
{
    std::string shape = "canvas-event-1";            ///< the record shape (codec.hpp)
    std::string order;                               ///< the order policy written in the segment header; empty: the shape's default
    Framing framing = Framing::frames;
    Layout layout = Layout::perWriter;
    Durability durability;
    std::uint64_t maxSegmentBytes = 64ull << 20;     ///< `frames`: a segment that would pass this is closed and the next begun; a `lines` stream is one file and ignores it
    bool lock = true;                                ///< hold an exclusive lock on the stream directory while open: a second writer is refused instead of corrupting it
};

/// The writer of one stream. Not thread-safe. Opening recovers: a torn tail left by a crash is cut off, and appending continues after the last good record.
class StoreWriter
{
public:
    StoreWriter(const StoreWriter&) = delete;
    StoreWriter& operator=(const StoreWriter&) = delete;
    StoreWriter(StoreWriter&&) noexcept = default;
    StoreWriter& operator=(StoreWriter&&) noexcept = default;
    ~StoreWriter();

    /// Open (creating it if new) the stream of `writer` under `root`. The errors: unreadable files, corruption before the tail (an append would bury it), an
    /// existing stream of another shape or writer, another process holding the lock.
    [[nodiscard]] static std::expected<StoreWriter, std::string> open(const std::filesystem::path& root, WriterId writer, WriterOptions options = {});

    /// Append `r`: its `seq` is set to the next position, and for a shape that carries its writer an empty writer is stamped with this one (another is an
    /// error). Returns the `seq`. Flushing follows the durability policy.
    [[nodiscard]] std::expected<std::uint64_t, std::string> append(Record& r);
    /// Append every record with one write per segment and one flush: the unit of durability and, later, of a git commit (review section 5). Returns the last `seq`.
    [[nodiscard]] std::expected<std::uint64_t, std::string> appendBatch(std::span<Record> records);
    /// Flush what was written to the device.
    [[nodiscard]] std::expected<void, std::string> sync();

    /// The `seq` the next appended record gets.
    [[nodiscard]] std::uint64_t nextSeq() const noexcept { return nextSeq_; }
    /// The number of the segment being appended to (1-based).
    [[nodiscard]] std::uint64_t segmentNumber() const noexcept { return segmentNumber_; }
    /// Bytes cut from the tail when this writer opened (0 when the stream ended cleanly).
    [[nodiscard]] std::uint64_t recoveredBytes() const noexcept { return recoveredBytes_; }
    [[nodiscard]] const WriterId& writer() const noexcept { return writer_; }

private:
    StoreWriter() = default;
    [[nodiscard]] std::expected<void, std::string> beginSegment(std::uint64_t number, std::uint64_t first);
    [[nodiscard]] std::expected<void, std::string> flushBuffer();
    [[nodiscard]] std::expected<void, std::string> afterAppend(bool batch);

    sub0platform::AppendFile file_;
    std::optional<sub0platform::FileLock> lock_;
    std::filesystem::path dir_;
    WriterId writer_;
    WriterOptions options_;
    const RecordCodec* codec_ = nullptr;
    std::string order_;
    std::uint64_t nextSeq_ = 1;
    std::uint64_t segmentNumber_ = 1;
    std::uint64_t segmentBytes_ = 0;
    std::uint64_t segmentRecords_ = 0;   ///< records in the segment being appended to, pending ones included
    std::uint64_t recoveredBytes_ = 0;
    std::string buffer_;   ///< encoded records not yet written
    bool dirty_ = false;
    std::chrono::steady_clock::time_point lastSync_{};
};

struct ReaderOptions
{
    std::uint32_t indexEvery = 64;        ///< the sparse index stride
    std::size_t chunkBytes = 64u << 10;   ///< the size of each read: the memory a stream holds is this and the largest record
};

/// What a reader has done, so a budget can be checked as a fact (review section 5) and not only as a time.
struct ReadStats
{
    std::uint64_t bytesRead = 0;          ///< bytes read from segment files, scans included
    std::uint64_t recordsScanned = 0;     ///< records checked while building or extending an index (no decode for frames)
    std::uint64_t recordsSkipped = 0;     ///< records passed over below a requested position, after an index seek
    std::uint64_t recordsReturned = 0;    ///< records handed out by the streams
};

/// One stream as the reader knows it.
struct StreamInfo
{
    WriterId writer;
    std::string shape;
    std::string order;
    Framing framing = Framing::frames;
    std::uint64_t records = 0;
    std::size_t segments = 0;
    std::uint64_t bytes = 0;
    bool tornTail = false;          ///< the last scan stopped at an unfinished record (a writer may be mid-write, or a crash left it)
    std::string damage;             ///< non-empty: corruption before the end, with where; reading that stream reports it
};

/// The merging reader over a store directory. Not thread-safe. It holds per stream and segment the sparse index and the count, built by one scan and
/// extended by `refresh`, never a rescan; a read holds one chunk per stream.
class StoreReader
{
public:
    StoreReader(StoreReader&&) noexcept;
    StoreReader& operator=(StoreReader&&) noexcept;
    ~StoreReader();
    StoreReader(const StoreReader&) = delete;
    StoreReader& operator=(const StoreReader&) = delete;

    /// Open the store at `root` and scan it (a missing `root` is an empty store). The error: an unreadable file or a stream that disagrees with its own
    /// header; corruption inside a segment is reported in `streams()` and by the stream that reads it.
    [[nodiscard]] static std::expected<StoreReader, std::string> open(const std::filesystem::path& root, ReaderOptions options = {});

    /// Find streams and segments written since, and extend the indexes over what grew. Streams made earlier by `since` read what they were made with.
    [[nodiscard]] std::expected<void, std::string> refresh();
    [[nodiscard]] std::vector<StreamInfo> streams() const;
    /// Records the stream of `writer` holds (as of the last scan); 0 when it is unknown.
    [[nodiscard]] std::uint64_t count(const WriterId& writer) const;

    /// Every record after `cursor`, merged across writers under the streams' order policy (`orderOverride`: that policy instead), as a pull stream that holds
    /// one chunk per stream: O(records returned) plus at most `indexEvery - 1` records skipped per stream after the index seek. The error: streams with
    /// different order policies (name one in `orderOverride`).
    [[nodiscard]] std::expected<std::unique_ptr<Merger>, std::string> since(const Cursor& cursor, std::string_view orderOverride = {}) const;

    [[nodiscard]] const ReadStats& stats() const noexcept { return *stats_; }

private:
    struct Impl;
    explicit StoreReader(std::unique_ptr<Impl> impl, std::shared_ptr<ReadStats> stats);
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<ReadStats> stats_;
};

}  // namespace sub0fabric::io
