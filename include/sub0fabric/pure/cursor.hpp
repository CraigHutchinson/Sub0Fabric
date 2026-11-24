#pragma once
/// @file cursor.hpp
/// A cursor: how far a reader has read, per writer. A merged stream has no single position that is safe when writers append at different times (a record
/// delivered late below an advanced position would be skipped: BACKBONE.md section 3.4, "per-writer cursors are the fix"), so a cursor holds one position per
/// writer: the number of that writer's records already consumed, which is the `seq` of the last one. `since(cursor)` yields exactly the rest, once.

#include "sub0fabric/pure/record.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>

namespace sub0fabric {

class Cursor
{
public:
    /// Records of `writer` already consumed; 0 for a writer the cursor has not seen.
    [[nodiscard]] std::uint64_t position(const WriterId& writer) const;
    /// Set the position of `writer`; a position of 0 removes the entry.
    void set(const WriterId& writer, std::uint64_t position);
    /// Note that `r` was consumed: the writer's position becomes `r.seq` when that is further than it is. Idempotent and monotone.
    void advance(const Record& r);
    /// Every writer the cursor has a position for, ordered by writer.
    [[nodiscard]] const std::map<WriterId, std::uint64_t>& positions() const noexcept { return positions_; }
    [[nodiscard]] bool empty() const noexcept { return positions_.empty(); }

    /// The durable text form, `cursor-1`: `{"format":"cursor-1","positions":[{"namespace":..,"writer":..,"position":N},...]}`, ordered by writer so equal
    /// cursors give equal bytes.
    [[nodiscard]] std::string toJson() const;
    /// Read a `cursor-1` text. The error names the first problem.
    [[nodiscard]] static std::expected<Cursor, std::string> fromJson(std::string_view text);

    bool operator==(const Cursor&) const = default;

private:
    std::map<WriterId, std::uint64_t> positions_;
};

}  // namespace sub0fabric
