#include "sub0fabric/io/store_query_source.hpp"

namespace sub0fabric::io {

std::vector<StreamSummary> StoreQuerySource::streams() const
{
    std::vector<StreamSummary> out;
    for (StreamInfo& info : reader_->streams()) out.push_back({std::move(info.writer), std::move(info.shape), std::move(info.order), info.records, std::move(info.damage)});
    return out;
}

std::expected<std::unique_ptr<RecordSource>, std::string> StoreQuerySource::since(const Cursor& cursor, std::string_view orderOverride) const
{
    auto merged = reader_->since(cursor, orderOverride);
    if (!merged) return std::unexpected(std::move(merged.error()));
    return std::unique_ptr<RecordSource>{std::move(*merged)};
}

}  // namespace sub0fabric::io
