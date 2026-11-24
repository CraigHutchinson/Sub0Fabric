#include "sub0fabric/pure/crc32c.hpp"

#include <array>
#include <bit>
#include <cstring>

namespace sub0fabric {

namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78u;   // reflected Castagnoli

using Tables = std::array<std::array<std::uint32_t, 256>, 8>;

constexpr Tables makeTables()
{
    Tables t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ kPolynomial : c >> 1;
        t[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i)
        for (std::size_t s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
    return t;
}

constexpr Tables kTables = makeTables();

}  // namespace

std::uint32_t crc32c(std::string_view data, std::uint32_t previous) noexcept
{
    std::uint32_t crc = ~previous;
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t n = data.size();
    while (n >= 8) {
        std::uint32_t lo = 0, hi = 0;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        if constexpr (std::endian::native == std::endian::big) {   // the tables assume little-endian loads
            lo = __builtin_bswap32(lo);
            hi = __builtin_bswap32(hi);
        }
        lo ^= crc;
        crc = kTables[7][lo & 0xFFu] ^ kTables[6][(lo >> 8) & 0xFFu] ^ kTables[5][(lo >> 16) & 0xFFu] ^ kTables[4][lo >> 24] ^
              kTables[3][hi & 0xFFu] ^ kTables[2][(hi >> 8) & 0xFFu] ^ kTables[1][(hi >> 16) & 0xFFu] ^ kTables[0][hi >> 24];
        p += 8;
        n -= 8;
    }
    while (n-- > 0) crc = (crc >> 8) ^ kTables[0][(crc ^ *p++) & 0xFFu];
    return ~crc;
}

}  // namespace sub0fabric
