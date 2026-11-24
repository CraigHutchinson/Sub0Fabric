#include "sub0fabric/pure/query_source.hpp"

#include "sub0fabric/pure/codec.hpp"

#include <format>

namespace sub0fabric {

std::expected<const OrderPolicy*, std::string> resolveReadOrder(std::span<const StreamSummary> streams, std::string_view orderOverride)
{
    std::string order{orderOverride};
    for (const StreamSummary& s : streams) {
        if (order.empty()) order = s.order;
        else if (orderOverride.empty() && s.order != order)
            return std::unexpected(std::format("streams are read in different orders ('{}' and '{}'): name one", order, s.order));
    }
    const OrderPolicy* policy = findOrderPolicy(order.empty() ? std::string_view{"lamport"} : std::string_view{order});
    if (policy == nullptr) return std::unexpected(std::format("unknown order policy '{}'", order));
    return policy;
}

std::expected<void, std::string> MemoryQuerySource::add(WriterId writer, std::string shape, std::vector<Record> records, std::string order)
{
    const RecordCodec* codec = findCodec(shape);
    if (codec == nullptr) return std::unexpected(std::format("unknown record shape '{}'", shape));
    if (order.empty()) order.assign(codec->defaultOrder());
    if (findOrderPolicy(order) == nullptr) return std::unexpected(std::format("unknown order policy '{}'", order));
    if (streams_.contains(writer)) return std::unexpected(std::format("stream '{}/{}' was added already", writer.ns, writer.writer));
    std::uint64_t seq = 0;
    for (Record& r : records) {
        if (r.writer == WriterId{}) r.writer = writer;
        else if (r.writer != writer)
            return std::unexpected(std::format("a record of '{}/{}' is in the stream of '{}/{}'", r.writer.ns, r.writer.writer, writer.ns, writer.writer));
        r.seq = ++seq;
    }
    streams_.emplace(std::move(writer), Stream{std::move(shape), std::move(order), std::move(records)});
    return {};
}

std::vector<StreamSummary> MemoryQuerySource::streams() const
{
    std::vector<StreamSummary> out;
    out.reserve(streams_.size());
    for (const auto& [writer, s] : streams_) out.push_back({writer, s.shape, s.order, s.records.size(), {}});
    return out;
}

std::expected<std::unique_ptr<RecordSource>, std::string> MemoryQuerySource::since(const Cursor& cursor, std::string_view orderOverride) const
{
    const std::vector<StreamSummary> summaries = streams();
    const auto policy = resolveReadOrder(summaries, orderOverride);
    if (!policy) return std::unexpected(policy.error());
    std::vector<std::unique_ptr<RecordSource>> sources;
    for (const auto& [writer, s] : streams_) {
        const std::uint64_t from = cursor.position(writer);   // records consumed: the next one has seq from + 1, which sits at index from
        if (from >= s.records.size()) continue;
        sources.push_back(std::make_unique<MemorySource>(std::vector<Record>(s.records.begin() + static_cast<std::ptrdiff_t>(from), s.records.end())));
    }
    return std::make_unique<Merger>(std::move(sources), **policy);
}

}  // namespace sub0fabric
