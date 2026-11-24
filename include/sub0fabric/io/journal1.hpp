#pragma once
/// @file journal1.hpp
/// Sub0Squad's `Journal` on Fabric (review D1: F0 is the journal lifted, behaviour-preserving). The same API, the same file (`journal-000001.ndjson` in the
/// directory, NDJSON lines `{"seq":N,"ts":T,"type":"..","d":{..}}`), the same recovery: a torn final line is dropped on open, damage before the end is an
/// error, a missing journal replays as empty. Squad's journal tests run against this class unchanged but for the include and the namespace
/// (`tests/test_journal1.cpp`). It is a compatibility facade over `StoreWriter`; new code uses the store directly.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sub0fabric::io {

class StoreWriter;

/// One replayed record. `body` is the raw JSON object stored under `d`.
struct JournalRecord
{
    std::uint64_t seq = 0;
    double ts = 0.0;  ///< wall-clock UTC seconds
    std::string type;
    std::string body;
};

/// Replay result for read-only consumers.
struct JournalReplay
{
    std::vector<JournalRecord> records;
    std::uint64_t validBytes = 0;  ///< file offset just after the last good record
    bool tornTail = false;         ///< a partial or unparseable final line was ignored
};

/// Writer. Not thread-safe. Holds every record in memory, as Squad's does; the merging reader is the streaming way to read a journal.
class Journal
{
public:
    Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&&) noexcept;
    Journal& operator=(Journal&&) noexcept;
    ~Journal();

    /// Read every good record without modifying the file. Error: unreadable file, or corruption before the final line.
    [[nodiscard]] static std::expected<JournalReplay, std::string> replay(const std::filesystem::path& dir);
    /// Replay, drop a torn tail from the file, and open for append. Takes no lock, as Squad's does (one daemon owns the journal).
    [[nodiscard]] static std::expected<Journal, std::string> open(const std::filesystem::path& dir);
    /// Append one record. `bodyJson` must be a JSON object. `durable` flushes to disk (state transitions); progress notes may skip it.
    [[nodiscard]] std::expected<std::uint64_t, std::string> append(std::string_view type, std::string_view bodyJson, bool durable = true);

    [[nodiscard]] const std::vector<JournalRecord>& records() const { return records_; }
    [[nodiscard]] std::uint64_t nextSeq() const { return nextSeq_; }

private:
    std::unique_ptr<StoreWriter> writer_;
    std::uint64_t nextSeq_ = 1;
    std::vector<JournalRecord> records_;
};

}  // namespace sub0fabric::io
