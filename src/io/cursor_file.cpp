#include "sub0fabric/io/cursor_file.hpp"

#include "sub0platform/file.hpp"

namespace sub0fabric::io {

std::expected<Cursor, std::string> loadCursor(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return Cursor{};
    const auto text = sub0platform::readFile(path);
    if (!text) return std::unexpected("cannot read " + path.string());
    auto cursor = Cursor::fromJson(*text);
    if (!cursor) return std::unexpected(path.string() + ": " + cursor.error());
    return cursor;
}

std::expected<void, std::string> saveCursor(const std::filesystem::path& path, const Cursor& cursor, bool durable)
{
    if (auto wrote = sub0platform::writeFileAtomic(path, cursor.toJson(), durable); !wrote) return std::unexpected(wrote.error().message);
    return {};
}

}  // namespace sub0fabric::io
