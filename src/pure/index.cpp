#include "sub0fabric/pure/index.hpp"

#include <algorithm>

namespace sub0fabric {

void SparseIndex::note(std::uint64_t position, std::uint64_t offset)
{
    if (entries_.empty() || position >= entries_.back().position + every_) entries_.push_back({position, offset});
}

SparseIndex::Entry SparseIndex::lookup(std::uint64_t position) const
{
    const auto it = std::ranges::upper_bound(entries_, position, {}, &Entry::position);
    if (it == entries_.begin()) return entries_.empty() ? Entry{} : entries_.front();
    return *(it - 1);
}

}  // namespace sub0fabric
