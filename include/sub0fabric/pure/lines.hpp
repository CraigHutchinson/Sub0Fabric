#pragma once
/// @file lines.hpp
/// JSON-lines framing over a `RequestHandler`: the whole of the stdio transport's logic, kept pure so it is tested without a process. The transport
/// reads bytes and writes bytes; this turns request lines into response lines.

#include "sub0fabric/pure/handler.hpp"

#include <string>
#include <string_view>

namespace sub0fabric {

/// Answer `input`, request lines separated by `\n` (a trailing `\r` is trimmed, so CRLF works): one response line, newline-terminated, per non-blank line,
/// in order. A blank line (empty or only whitespace) gets no response. A response never contains a raw newline (one that did is flattened to spaces, which
/// JSON allows between tokens). A line over the handler's request limit is the handler's `too-large` answer: this function does not cut lines.
[[nodiscard]] std::string serveLines(RequestHandler& handler, std::string_view input, const RequestContext& context);

}  // namespace sub0fabric
