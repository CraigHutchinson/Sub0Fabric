#include "sub0fabric/pure/layout.hpp"

#include <charconv>
#include <format>

namespace sub0fabric {

namespace {

bool plain(char c, bool first) noexcept
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') return true;
    return c == '.' && !first;
}

int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

}  // namespace

std::string encodeName(std::string_view id)
{
    if (id.empty()) return "~";
    std::string out;
    out.reserve(id.size());
    bool first = true;
    for (const char c : id) {
        if (plain(c, first)) {
            out.push_back(c);
        } else {
            out += std::format("%{:02X}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        }
        first = false;
    }
    return out;
}

std::expected<std::string, std::string> decodeName(std::string_view name)
{
    if (name == "~") return std::string{};
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] != '%') { out.push_back(name[i]); continue; }
        if (i + 2 >= name.size()) return std::unexpected("truncated %-escape in a name");
        const int hi = hexValue(name[i + 1]), lo = hexValue(name[i + 2]);
        if (hi < 0 || lo < 0) return std::unexpected("bad %-escape in a name");
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return out;
}

std::string segmentFileName(std::uint64_t index) { return std::format("seg-{:06}.s0f", index); }

std::optional<std::uint64_t> parseSegmentFileName(std::string_view name) noexcept
{
    constexpr std::string_view kPrefix = "seg-", kSuffix = ".s0f";
    if (name.size() <= kPrefix.size() + kSuffix.size() || !name.starts_with(kPrefix) || !name.ends_with(kSuffix)) return std::nullopt;
    const std::string_view digits = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
    std::uint64_t n = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (ec != std::errc{} || end != digits.data() + digits.size() || n == 0) return std::nullopt;
    return n;
}

}  // namespace sub0fabric
