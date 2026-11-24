#pragma once
/// @file store_query_source.hpp
/// The directory store as a `QuerySource` (pure/query_source.hpp): the second implementation of the seam, so the protocol is proved over memory and over
/// files with the same golden cases and byte-identical answers.

#include "sub0fabric/io/store.hpp"
#include "sub0fabric/pure/query_source.hpp"

namespace sub0fabric::io {

/// A query source over a `StoreReader`. It holds the reader by reference (it must outlive the source); the owner refreshes the reader (`refresh()`) when it
/// wants new records visible, and the protocol sees them on its next request. Not thread-safe, as the reader is not.
class StoreQuerySource final : public QuerySource
{
public:
    explicit StoreQuerySource(const StoreReader& reader) : reader_(&reader) {}

    /// The reader's streams, ordered by writer, each with its shape, order, record count and damage.
    [[nodiscard]] std::vector<StreamSummary> streams() const override;
    /// `StoreReader::since`: O(records returned) through the sparse index; an unreadable or damaged segment is the error of the read that reaches it.
    [[nodiscard]] std::expected<std::unique_ptr<RecordSource>, std::string> since(const Cursor& cursor, std::string_view orderOverride) const override;

private:
    const StoreReader* reader_;
};

}  // namespace sub0fabric::io
