#include "sub0fabric/pure/cursor.hpp"

#include "json_util.hpp"

#include <format>

namespace sub0fabric {

std::uint64_t Cursor::position(const WriterId& writer) const
{
    const auto it = positions_.find(writer);
    return it == positions_.end() ? 0 : it->second;
}

void Cursor::set(const WriterId& writer, std::uint64_t position)
{
    if (position == 0) positions_.erase(writer);
    else positions_[writer] = position;
}

void Cursor::advance(const Record& r)
{
    std::uint64_t& p = positions_[r.writer];
    if (r.seq > p) p = r.seq;
    if (p == 0) positions_.erase(r.writer);
}

std::string Cursor::toJson() const
{
    // The array is assembled by hand around one small object per writer (the writer helper has no arrays): positions are few and the text is small.
    std::string text = R"({"format":"cursor-1","positions":[)";
    bool first = true;
    for (const auto& [writer, position] : positions_) {
        json::Writer m;
        m.beginObject().kv("namespace", writer.ns).kv("writer", writer.writer).kv("position", static_cast<std::int64_t>(position)).endObject();
        if (!first) text += ',';
        first = false;
        text.append(m.view());
    }
    text += "]}";
    return text;
}

std::expected<Cursor, std::string> Cursor::fromJson(std::string_view text)
{
    Cursor c;
    bool formatOk = false;
    try {
        json::ParseSession session;
        auto doc = session.iterate(text);
        simdjson::ondemand::object obj = doc.get_object();
        for (auto field : obj) {
            const std::string_view key = field.unescaped_key();
            auto value = field.value();
            if (key == "format") {
                std::string_view f = value.get_string();
                formatOk = f == "cursor-1";
            } else if (key == "positions") {
                for (auto item : value.get_array()) {
                    WriterId id;
                    std::uint64_t position = 0;
                    for (auto f : item.get_object()) {
                        const std::string_view k = f.unescaped_key();
                        auto v = f.value();
                        if (k == "namespace") { std::string_view s = v.get_string(); id.ns.assign(s); }
                        else if (k == "writer") { std::string_view s = v.get_string(); id.writer.assign(s); }
                        else if (k == "position") position = v.get_uint64();
                    }
                    c.set(id, position);
                }
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        return std::unexpected(std::format("not a cursor: {}", e.what()));
    }
    if (!formatOk) return std::unexpected("not a cursor-1 text (format member missing or different)");
    return c;
}

}  // namespace sub0fabric
