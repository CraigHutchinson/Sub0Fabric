#pragma once
/// @file codec.hpp
/// Record shapes: how a Record is written to and read from the bytes of one frame or line (review D2: the record layer, kinds as data). Two shapes are
/// proved, with disjoint wire field sets so a line tells which one it is: `journal-1` (Sub0Squad's job journal) and `canvas-event-1` (Sub0Canvas's
/// event envelope, BACKBONE.md section 3.3, amendment A22). A third (the read-only Sub0Log adapter) is a codec like these.

#include "sub0fabric/pure/record.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace sub0fabric {

/// A record shape. Stateless and thread-safe; one instance per shape, found by name.
class RecordCodec
{
public:
    virtual ~RecordCodec() = default;

    /// The shape's name, stored in a segment header: `journal-1`, `canvas-event-1`.
    [[nodiscard]] virtual std::string_view shape() const noexcept = 0;
    /// The order policy (order.hpp) a stream of this shape is read in unless its segment header says otherwise: `seq` or `lamport`.
    [[nodiscard]] virtual std::string_view defaultOrder() const noexcept = 0;
    /// Whether the wire form carries `seq` (journal-1): then a reader checks it equals the record's position, and a writer must set it.
    [[nodiscard]] virtual bool carriesSeq() const noexcept = 0;
    /// Whether the wire form carries the writer (canvas): if not (journal-1), the stream's writer is stamped on each record read from it.
    [[nodiscard]] virtual bool carriesWriter() const noexcept = 0;
    /// Every top-level field name of the wire form (an unknown member of a shape that keeps them is not listed).
    [[nodiscard]] virtual std::span<const std::string_view> fields() const noexcept = 0;
    /// The fields without which a text is not this shape.
    [[nodiscard]] virtual std::span<const std::string_view> requiredFields() const noexcept = 0;

    /// Append the wire form of `r` to `out`: one JSON object, no newline.
    virtual void encode(const Record& r, std::string& out) const = 0;
    /// Read one wire form into `out`, replacing its members the shape carries. The error names the first problem.
    [[nodiscard]] virtual std::expected<void, std::string> decode(std::string_view text, Record& out) const = 0;
};

/// The shape called `name`, or nullptr.
[[nodiscard]] const RecordCodec* findCodec(std::string_view name) noexcept;
/// Every shape this build knows.
[[nodiscard]] std::span<const RecordCodec* const> codecs() noexcept;

/// Which shape `text` (one JSON object) is, by its top-level fields: the one whose required fields are all present. An error when none or several match.
[[nodiscard]] std::expected<std::string_view, std::string> detectShape(std::string_view text);

/// True when no two shapes share a wire field name: the property that makes `detectShape` possible (and answers the review's question whether the
/// journal and the canvas envelope collide: they do not). A new shape must keep it, and a test holds it.
[[nodiscard]] bool fieldSetsDisjoint() noexcept;

}  // namespace sub0fabric
