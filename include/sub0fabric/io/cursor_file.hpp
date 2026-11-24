#pragma once
/// @file cursor_file.hpp
/// Durable cursors (review D2 `cursor`): a reader's position, one number per writer, kept in a file it replaces atomically, so a restart resumes with
/// `StoreReader::since(cursor)` and neither misses nor repeats a record.

#include "sub0fabric/pure/cursor.hpp"

#include <expected>
#include <filesystem>
#include <string>

namespace sub0fabric::io {

/// Read the cursor in `path`; a missing file is the empty cursor (read from the start). The error: an unreadable or malformed file.
[[nodiscard]] std::expected<Cursor, std::string> loadCursor(const std::filesystem::path& path);

/// Replace the cursor in `path` through a temporary file and a rename: a crash leaves the old cursor or the new, never half. `durable`: flushed to the
/// device before the rename. A cursor is advanced only after the reader has handled the records, so a crash repeats work rather than losing it.
[[nodiscard]] std::expected<void, std::string> saveCursor(const std::filesystem::path& path, const Cursor& cursor, bool durable = true);

}  // namespace sub0fabric::io
