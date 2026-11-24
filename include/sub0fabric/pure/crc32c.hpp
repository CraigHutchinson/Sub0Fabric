#pragma once
/// @file crc32c.hpp
/// CRC-32C (Castagnoli), the checksum of every frame and segment header. Table-driven, slice-by-8, no hardware dependence: the same value on every machine.

#include <cstdint>
#include <string_view>

namespace sub0fabric {

/// The CRC-32C of `data`, continuing from `previous` (the result of an earlier call; 0 starts a new sum), so `crc32c(b, crc32c(a)) == crc32c(a + b)`.
[[nodiscard]] std::uint32_t crc32c(std::string_view data, std::uint32_t previous = 0) noexcept;

}  // namespace sub0fabric
