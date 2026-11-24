#pragma once
/// @file query_source.hpp
/// What the protocol (protocol.hpp) reads: the streams a log holds and a merged read of what follows a cursor. The seam has two implementations that
/// must answer alike: `MemoryQuerySource` here (records held in memory: tests, the reference) and `StoreQuerySource` over the directory store
/// (io/store_query_source.hpp). A third (a live ring, a git-ref store) is one more class; the protocol does not change.

#include "sub0fabric/pure/cursor.hpp"
#include "sub0fabric/pure/order.hpp"
#include "sub0fabric/pure/record.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sub0fabric {

/// One stream as a query source reports it.
struct StreamSummary
{
    WriterId writer;
    std::string shape;          ///< the record shape (codec.hpp) the stream's records are written in
    std::string order;          ///< the order policy name the stream is read in unless a read overrides it
    std::uint64_t records = 0;  ///< how many records the stream holds
    std::string damage;         ///< non-empty: corruption before the end, with where (the stream's read reports it)
};

/// The read side the protocol needs. Not thread-safe unless an implementation says so; a response is computed from one `streams()` and one `since()`.
class QuerySource
{
public:
    virtual ~QuerySource() = default;

    /// Every stream, ordered by writer (namespace, then writer id): the order of the `streams` listing, which a cursor over it relies on.
    [[nodiscard]] virtual std::vector<StreamSummary> streams() const = 0;

    /// Every record after `cursor` (per writer: those whose `seq` is above its position), merged across writers under the streams' order policy, or
    /// under `orderOverride` when it is not empty, as a pull source. The error: streams with different order policies and no override, an unknown policy name.
    [[nodiscard]] virtual std::expected<std::unique_ptr<RecordSource>, std::string> since(const Cursor& cursor, std::string_view orderOverride) const = 0;
};

/// The order policy a read of `streams` uses: `orderOverride` when it is not empty, else the one every stream shares (`lamport` when there are none). The
/// error for streams that disagree names two of the orders (the same text the directory store gives) or says the policy name is unknown.
[[nodiscard]] std::expected<const OrderPolicy*, std::string> resolveReadOrder(std::span<const StreamSummary> streams, std::string_view orderOverride);

/// A query source over records held in memory: the reference implementation of the seam.
class MemoryQuerySource final : public QuerySource
{
public:
    /// Add the stream of `writer` holding `records` in `shape`: each record's `seq` is set to its position 1..n, and an empty record writer is stamped with
    /// `writer` (a different one is an error). `order` empty means the shape's default order. The error: an unknown shape or order, a writer added twice.
    [[nodiscard]] std::expected<void, std::string> add(WriterId writer, std::string shape, std::vector<Record> records, std::string order = {});

    [[nodiscard]] std::vector<StreamSummary> streams() const override;
    [[nodiscard]] std::expected<std::unique_ptr<RecordSource>, std::string> since(const Cursor& cursor, std::string_view orderOverride) const override;

private:
    struct Stream
    {
        std::string shape;
        std::string order;
        std::vector<Record> records;
    };
    std::map<WriterId, Stream> streams_;
};

}  // namespace sub0fabric
