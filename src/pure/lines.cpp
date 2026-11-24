#include "sub0fabric/pure/lines.hpp"

#include <algorithm>

namespace sub0fabric {

namespace {

bool blank(std::string_view s)
{
    return std::ranges::all_of(s, [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
}

}  // namespace

std::string serveLines(RequestHandler& handler, std::string_view input, const RequestContext& context)
{
    std::string out;
    while (!input.empty()) {
        const std::size_t eol = input.find('\n');
        std::string_view line = input.substr(0, eol);
        input = eol == std::string_view::npos ? std::string_view{} : input.substr(eol + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (blank(line)) continue;
        std::string response = handler.handle(line, context);
        std::ranges::replace(response, '\n', ' ');   // JSON allows whitespace between tokens, never inside a string, so this cannot change a valid response
        std::ranges::replace(response, '\r', ' ');
        out += response;
        out += '\n';
    }
    return out;
}

}  // namespace sub0fabric
