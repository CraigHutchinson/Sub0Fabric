#pragma once
/// @file layout.hpp
/// Names on disk, as pure string functions so the io layer and its tests agree: a writer's directory is `<root>/<ns>/<writer>/`, its segments
/// `seg-000001.s0f`, `seg-000002.s0f`, and a journal-1 directory holds `journal-000001.ndjson`.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace sub0fabric {

/// A namespace or writer id as one path component: letters, digits and `._-` stay, every other byte becomes `%XX` (upper-case hex), the empty string
/// is `~`, and a leading `.` is encoded so no name is `.` or `..`. Distinct ids give distinct names (the encoding is reversible).
[[nodiscard]] std::string encodeName(std::string_view id);
/// The id an `encodeName` result stands for. The error: a malformed escape.
[[nodiscard]] std::expected<std::string, std::string> decodeName(std::string_view name);

/// `seg-NNNNNN.s0f` for segment number `index` (1-based; six digits, more when needed).
[[nodiscard]] std::string segmentFileName(std::uint64_t index);
/// The number in a segment file name, or nullopt for any other name.
[[nodiscard]] std::optional<std::uint64_t> parseSegmentFileName(std::string_view name) noexcept;

/// The file of a journal-1 directory (Sub0Squad's name, kept so an existing journal opens unchanged).
inline constexpr std::string_view kJournalFileName = "journal-000001.ndjson";

}  // namespace sub0fabric
