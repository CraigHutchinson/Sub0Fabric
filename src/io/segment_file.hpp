#pragma once
// Internal: reading segment files in chunks for the writer's recovery and the reader's index. Not part of the public API.
#include "sub0fabric/pure/index.hpp"
#include "sub0fabric/pure/segment.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace sub0fabric::io::detail {

/// The outcome of scanning a segment file from an offset to its end.
struct FileScan
{
    std::uint64_t validOffset = 0;   ///< just after the last good record
    std::uint64_t position = 0;      ///< the 0-based position of the next record: `first` plus the records found
    std::uint64_t fileSize = 0;
    bool torn = false;               ///< the file ends in an unfinished record
    std::string corrupt;             ///< non-empty: damage before the end (the scanner's message)
    std::uint64_t bytesRead = 0;
    std::uint64_t records = 0;
};

/// Scan `path` from `startOffset`, checking records without decoding them where the framing allows (a `frames` payload is checked by its checksum), noting
/// every record start in `index` when given. `firstPosition` is the position of the record at `startOffset`. An error only when the file cannot be read.
[[nodiscard]] std::expected<FileScan, std::string> scanFile(const std::filesystem::path& path, Framing framing, const RecordCodec& codec, std::uint64_t firstPosition,
                                                            std::uint64_t startOffset, SparseIndex* index, std::size_t chunkBytes);

/// Read the header of a `frames` segment file. `size` is the file's size.
struct FileHeader
{
    HeaderParse parse;
    std::uint64_t fileSize = 0;
};
[[nodiscard]] std::expected<FileHeader, std::string> readHeader(const std::filesystem::path& path);

/// The first line of a `lines` file (up to 64 KiB), or empty.
[[nodiscard]] std::expected<std::string, std::string> readFirstLine(const std::filesystem::path& path);

/// The segment files of a directory (`seg-NNNNNN.s0f`), ascending by number.
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::filesystem::path>> listSegments(const std::filesystem::path& dir);

}  // namespace sub0fabric::io::detail
