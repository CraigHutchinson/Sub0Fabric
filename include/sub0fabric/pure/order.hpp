#pragma once
/// @file order.hpp
/// Order is data (review D4): a stream's order rule is a name in its segment header, resolved here to a comparison, not a constant of the library. One
/// streaming k-way merge serves every policy. Policies: `seq` (Sub0Squad's journal: position, then writer) and `lamport` (Sub0Canvas: lamport clock, then
/// writer, review O3). `aligned-time` (Sub0Log, anchored wall time) is named in the review and arrives with the Sub0Log adapter.

#include "sub0fabric/pure/record.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sub0fabric {

/// A named total order on records. `less` must be a strict weak order that is total on the records of distinct (writer, seq) pairs, so a merge is the same
/// on every machine and every run.
struct OrderPolicy
{
    std::string_view name;
    bool (*less)(const Record& a, const Record& b) noexcept;
};

/// The policy called `name` (`seq`, `lamport`), or nullptr.
[[nodiscard]] const OrderPolicy* findOrderPolicy(std::string_view name) noexcept;
/// Every policy this build knows.
[[nodiscard]] std::span<const OrderPolicy> orderPolicies() noexcept;

/// A pull source of records in one stream's own order (position order within one writer), which is also its order under any policy the stream is read in.
class RecordSource
{
public:
    virtual ~RecordSource() = default;
    /// Read the next record into `out`: true when one was read, false at the end, an error when the stream is damaged before its end.
    [[nodiscard]] virtual std::expected<bool, std::string> next(Record& out) = 0;
};

/// A source over records held in memory: tests, benchmarks, and the reference implementation of the seam.
class MemorySource final : public RecordSource
{
public:
    explicit MemorySource(std::vector<Record> records) : records_(std::move(records)) {}
    [[nodiscard]] std::expected<bool, std::string> next(Record& out) override;

private:
    std::vector<Record> records_;
    std::size_t next_ = 0;
};

/// A streaming k-way merge of sources under an order policy. Memory is one head record per source (bounded in the number of sources, never in the number
/// of records, review section 5), and a record is produced in O(log k). A source that is not itself in policy order does not stop the merge: the
/// violation is counted (`outOfOrder`) and the merge continues, so the output is then not globally sorted.
class Merger final : public RecordSource
{
public:
    Merger(std::vector<std::unique_ptr<RecordSource>> sources, const OrderPolicy& policy);

    /// The next record in policy order; false at the end of every source; an error when a source reports one: after every record that precedes the damage was handed out, and the merge then stops.
    [[nodiscard]] std::expected<bool, std::string> next(Record& out) override;
    /// How many records came out below the record before them from the same source.
    [[nodiscard]] std::uint64_t outOfOrder() const noexcept { return outOfOrder_; }
    [[nodiscard]] std::size_t sourceCount() const noexcept { return sources_.size(); }

private:
    struct Head
    {
        Record record;
        std::size_t source = 0;
    };
    [[nodiscard]] bool after(const Head& a, const Head& b) const noexcept;

    std::vector<std::unique_ptr<RecordSource>> sources_;
    const OrderPolicy* policy_;
    std::vector<Head> heap_;
    bool primed_ = false;
    std::optional<std::string> failure_;   ///< a source's error, reported after the last record that precedes it
    std::uint64_t outOfOrder_ = 0;
};

}  // namespace sub0fabric
