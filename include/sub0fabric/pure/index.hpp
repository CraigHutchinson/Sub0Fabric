#pragma once
/// @file index.hpp
/// The sparse offset index of one segment (review section 5: `since(cursor)` is O(records returned) via a sparse offset index per segment, never a rescan).
/// It holds the file offset of every `every`-th record, so a reader seeking to position p starts at the nearest entry at or before p and decodes at most
/// `every - 1` records it does not return. It is a disposable cache (review A19): rebuilt by one scan, extended as the segment grows.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sub0fabric {

class SparseIndex
{
public:
    /// One entry: the record at `position` (0-based, in the writer's stream) starts at file offset `offset`.
    struct Entry
    {
        std::uint64_t position = 0;
        std::uint64_t offset = 0;
    };

    /// `every`: the stride between entries (at least 1).
    explicit SparseIndex(std::uint32_t every = 64) : every_(every == 0 ? 1 : every) {}

    /// Note that the record at `position` starts at `offset`. Positions must be given in increasing order; one is kept when it is the first or at least
    /// `every` past the last kept.
    void note(std::uint64_t position, std::uint64_t offset);
    /// The entry to start reading from to reach `position`: the last entry at or before it, or the first entry when `position` is before them all.
    /// The index must hold at least one entry (note the segment's first record).
    [[nodiscard]] Entry lookup(std::uint64_t position) const;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::uint32_t every() const noexcept { return every_; }
    /// Heap bytes held, for the memory metric: entries, not records.
    [[nodiscard]] std::size_t bytes() const noexcept { return entries_.capacity() * sizeof(Entry); }

private:
    std::uint32_t every_;
    std::vector<Entry> entries_;
};

}  // namespace sub0fabric
