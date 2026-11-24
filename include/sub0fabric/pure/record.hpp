#pragma once
/// @file record.hpp
/// The record: one entry of an append-only log, in the neutral form every record shape (codec.hpp) reads into and writes from.

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace sub0fabric {

/// Who wrote a record: a namespace (project, source home) and a writer id local to it, because two homes both mint `w-000001` (review D6).
/// Ordered by namespace, then writer: the tie-break of every merge order.
struct WriterId
{
    std::string ns;       ///< project or source home; may be empty
    std::string writer;   ///< the writer's id within the namespace
    auto operator<=>(const WriterId&) const = default;
};

/// One record. The wire form is a shape's business (`journal-1`, `canvas-event-1`); a shape fills the members it carries and leaves the others at
/// their defaults, so two shapes with disjoint wire fields share one in-memory type and one merge.
struct Record
{
    std::string kind;            ///< journal-1 `type`; canvas `kind`, `name@version`
    WriterId writer;             ///< canvas: in the record; journal-1: the stream's writer (the line does not carry it)
    std::uint64_t seq = 0;       ///< 1-based position in the writer's stream: set by the segment on read and append; journal-1 also writes it
    std::uint64_t lamport = 0;   ///< canvas logical clock, strictly increasing per writer; 0 for journal-1
    double ts = 0.0;             ///< journal-1 wall-clock UTC seconds
    std::string subject;         ///< canvas: an item key, a block id, a path, a claim id
    std::string time;            ///< canvas: ISO 8601, for display only: it never orders anything
    std::string payload = "{}";  ///< the kind's data as raw JSON, kept verbatim: a reader that does not know the kind never rewrites it
    std::string extra;           ///< canvas: members this version does not know, as raw JSON members (`"k":v,...`), kept verbatim and written back
    bool operator==(const Record&) const = default;
};

/// A kind split at its `@`: `item.put@1` is name `item.put`, version `1`. A kind without `@` has an empty version.
struct KindRef
{
    std::string_view name, version;
};
[[nodiscard]] KindRef splitKind(std::string_view kind) noexcept;

}  // namespace sub0fabric
