#include "sub0fabric/pure/order.hpp"

#include <algorithm>
#include <array>

namespace sub0fabric {

namespace {

bool seqLess(const Record& a, const Record& b) noexcept
{
    if (a.seq != b.seq) return a.seq < b.seq;
    return a.writer < b.writer;
}

bool lamportLess(const Record& a, const Record& b) noexcept
{
    if (a.lamport != b.lamport) return a.lamport < b.lamport;
    if (a.writer != b.writer) return a.writer < b.writer;
    return a.seq < b.seq;   // a writer never reuses a lamport value; this keeps the order total when a damaged stream does
}

constexpr std::array<OrderPolicy, 2> kPolicies{{{"seq", &seqLess}, {"lamport", &lamportLess}}};

}  // namespace

const OrderPolicy* findOrderPolicy(std::string_view name) noexcept
{
    for (const OrderPolicy& p : kPolicies)
        if (p.name == name) return &p;
    return nullptr;
}

std::span<const OrderPolicy> orderPolicies() noexcept { return kPolicies; }

std::expected<bool, std::string> MemorySource::next(Record& out)
{
    if (next_ >= records_.size()) return false;
    out = std::move(records_[next_++]);
    return true;
}

Merger::Merger(std::vector<std::unique_ptr<RecordSource>> sources, const OrderPolicy& policy) : sources_(std::move(sources)), policy_(&policy) {}

bool Merger::after(const Head& a, const Head& b) const noexcept
{
    if (policy_->less(b.record, a.record)) return true;
    if (policy_->less(a.record, b.record)) return false;
    return a.source > b.source;   // equal under the policy: the earlier source first, so the merge is deterministic
}

std::expected<bool, std::string> Merger::next(Record& out)
{
    const auto cmp = [this](const Head& a, const Head& b) { return after(a, b); };
    if (!primed_) {
        primed_ = true;
        heap_.reserve(sources_.size());
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            Head h;
            h.source = i;
            auto got = sources_[i]->next(h.record);
            if (!got) return std::unexpected(got.error());
            if (*got) heap_.push_back(std::move(h));
        }
        std::ranges::make_heap(heap_, cmp);
    }
    if (heap_.empty()) return false;
    std::ranges::pop_heap(heap_, cmp);
    Head& top = heap_.back();
    out = std::move(top.record);
    auto got = sources_[top.source]->next(top.record);
    if (!got) return std::unexpected(got.error());
    if (*got) {
        if (policy_->less(top.record, out)) ++outOfOrder_;
        std::ranges::push_heap(heap_, cmp);
    } else {
        heap_.pop_back();
    }
    return true;
}

}  // namespace sub0fabric
