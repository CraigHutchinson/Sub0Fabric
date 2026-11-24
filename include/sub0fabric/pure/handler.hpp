#pragma once
/// @file handler.hpp
/// The request seam of every transport (review D7: protocol first, independent of transport): one request text in, one response text out, and who is
/// asking. A transport (stdio lines, HTTP, an in-process call) owns the framing and the identity; a handler owns the meaning. `ProtocolServer`
/// (protocol.hpp) is the first handler; the projecting handler of W3b is a second that wraps it.

#include "sub0fabric/pure/record.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace sub0fabric {

/// Who is asking, as the transport knows it. Identity comes from the transport (a socket peer, a token it checked, the process's own user), never from the
/// request: a request member that names an actor is just an unknown member (contract P-7).
struct RequestContext
{
    WriterId actor;                          ///< the authenticated caller; empty for an in-process or local-owner caller
    std::vector<std::string> capabilities;   ///< what the transport granted the caller, by name; the meaning of a name belongs to the handler (W3b projection)
};

/// A request handler. Pure: the same request, context and source state give the same response bytes. Never throws; every failure is a response.
class RequestHandler
{
public:
    virtual ~RequestHandler() = default;

    /// Answer one request (one JSON object, no framing) as one response (one JSON object with no raw newline). Failures are structured error responses,
    /// never an empty reply.
    [[nodiscard]] virtual std::string handle(std::string_view request, const RequestContext& context) = 0;
};

}  // namespace sub0fabric
