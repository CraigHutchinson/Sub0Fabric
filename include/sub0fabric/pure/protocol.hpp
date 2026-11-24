#pragma once
/// @file protocol.hpp
/// The query protocol `fabric-query-1` (docs/DESIGN.md "Protocol (W3a)", the one home of the schema): a JSON request in, a JSON response out, over a
/// `QuerySource`. It is pure: no transport, no I/O, no clock. A transport (stdio lines, HTTP) calls `ProtocolServer::handle` and owns framing and identity.
///
/// The envelope contract (`schema`, `ok`, `op`, `id`, `error{code,field,message}`, `limit`/`clamped`, `next`, `maxResponseBytes`, `unknownFields`) is the one
/// Sub0Canvas's `query-1` defines; Fabric shares the contract, not code (Fabric depends on no other Sub0 project), and the schema name is its own.

#include "sub0fabric/pure/handler.hpp"
#include "sub0fabric/pure/query_source.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace sub0fabric {

/// The schema name every response carries and every request may carry.
inline constexpr std::string_view kProtocolSchema = "fabric-query-1";

/// What a request member this version does not know gets: `ignore` (answered as if absent, byte for byte), `echo` (answered, and the names listed in
/// `unknownFields`: at most 16, each clipped to 64 characters, in request order), `reject` (error `unknown-field` naming the first).
enum class UnknownFields
{
    ignore,
    echo,
    reject,
};

/// The protocol's bounds and defaults, as data a client or an operator may supply (`parseProtocolPolicy`) and `info` reports.
struct ProtocolPolicy
{
    /// A list operation's page size: used when the request has no `limit`, and the most a request may ask for (a larger `limit` is clamped).
    struct Page
    {
        std::uint32_t defaultSize, maxSize;
    };
    Page streams{100, 1000};                           ///< the `streams` listing
    Page since{200, 2000};                             ///< the `since` read
    std::size_t maxResponseBytes = 1u << 20;           ///< a page stops adding entries once its entries reach this (the first entry is always added)
    std::size_t maxRequestBytes = 64u << 10;           ///< a longer request is `too-large`, before it is parsed
    UnknownFields unknownFields = UnknownFields::echo; ///< see `UnknownFields`
};

/// Read a policy from JSON: `{"streams":{"default":N,"max":N},"since":{..},"maxResponseBytes":N,"maxRequestBytes":N,"unknownFields":"echo"}`. Every
/// member is optional (absent: the default above). An unknown member, a zero or non-numeric size, or a default above its maximum is an error naming the member.
[[nodiscard]] std::expected<ProtocolPolicy, std::string> parseProtocolPolicy(std::string_view json);
/// The policy as the JSON `parseProtocolPolicy` reads and `info` reports; `json(parse(json(p))) == json(p)`.
[[nodiscard]] std::string protocolPolicyJson(const ProtocolPolicy& policy);

/// What a server has done, added to by each call that is given one: the deterministic facts a budget is checked against.
struct ProtocolStats
{
    std::uint64_t recordsReturned = 0;   ///< `since` entries sent
    std::uint64_t streamsListed = 0;     ///< `streams` entries sent
    std::uint64_t responseBytes = 0;     ///< bytes of every response, errors included
};

/// The `fabric-query-1` handler over a query source. Stateless between requests (a cursor is a place the client holds), so two servers over one source
/// answer alike and a request may be repeated. Not thread-safe unless the source is.
class ProtocolServer final : public RequestHandler
{
public:
    /// `source` must outlive the server.
    explicit ProtocolServer(const QuerySource& source, ProtocolPolicy policy = {});

    /// Answer one request as one response with no raw newline. `context` is the transport's identity of the caller; W3a answers every caller alike
    /// (the projecting handler of W3b is what reads it) and never takes identity from the request (contract P-7).
    [[nodiscard]] std::string handle(std::string_view request, const RequestContext& context) override;
    /// The same, adding what the call did to `*stats` (when not null).
    [[nodiscard]] std::string handle(std::string_view request, const RequestContext& context, ProtocolStats* stats);

    [[nodiscard]] const ProtocolPolicy& policy() const noexcept { return policy_; }

private:
    const QuerySource* source_;
    ProtocolPolicy policy_;
};

}  // namespace sub0fabric
