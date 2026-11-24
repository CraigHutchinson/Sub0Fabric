#include "sub0fabric/pure/protocol.hpp"

#include "json_util.hpp"
#include "sub0fabric/pure/codec.hpp"
#include "sub0fabric/pure/cursor.hpp"
#include "sub0fabric/pure/order.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace sub0fabric {

namespace {

constexpr std::array<std::string_view, 3> kOps{"info", "streams", "since"};
constexpr std::size_t kMaxEchoedNames = 16;     // contract: unknownFields lists at most this many names
constexpr std::size_t kMaxEchoedChars = 64;     // ... each clipped to this many characters

/// A failure that becomes the `error` member of a response.
struct Err
{
    std::string code;      ///< bad-request, unsupported-schema, unknown-op, unknown-field, bad-cursor, too-large, mixed-order, store-error
    std::string field;     ///< the request member at fault; empty when it is the request as a whole
    std::string message;
};

/// What a request said, with every malformed member remembered rather than raised, so the checks run in one fixed order whatever order the members came in.
struct Parsed
{
    std::optional<std::string> schema;
    bool schemaBad = false;
    std::optional<std::string> op;
    bool opBad = false;
    std::optional<std::string> idRaw;   ///< the id's JSON token as it came: a string with its quotes, or a whole number
    bool idBad = false;
    std::optional<std::uint64_t> limit;
    bool limitBad = false;
    std::optional<std::string> cursorRaw;   ///< the cursor's JSON object as it came; absent when missing or null
    bool cursorBad = false;
    std::optional<std::string> order;
    bool orderBad = false;
    std::vector<std::string> names;   ///< every member name, in request order
};

/// What an error response may echo of the request: the `op` and `id` that were well formed.
struct Echo
{
    std::optional<std::string> op;
    std::optional<std::string> idRaw;
};

/// The members of a successful answer's payload, and the counts a caller adds to its stats.
struct Payload
{
    std::string members;   ///< JSON members without braces
    std::uint64_t records = 0;
    std::uint64_t streams = 0;
};

std::string errorJson(const Echo& echo, const Err& e)
{
    json::Writer w;
    w.beginObject().kv("schema", kProtocolSchema).kvRaw("ok", "false");
    if (echo.op) w.kv("op", *echo.op);
    if (echo.idRaw) w.kvRaw("id", *echo.idRaw);
    w.key("error").beginObject().kv("code", e.code).kv("field", e.field).kv("message", e.message).endObject();
    w.endObject();
    return std::string{w.view()};
}

/// A JSON object's members without its braces, for splicing into a larger object.
std::string membersOf(json::Writer& object)
{
    const std::string_view whole = object.view();
    return std::string{whole.substr(1, whole.size() - 2)};
}

/// `name` cut to at most `kMaxEchoedChars` characters (code points: a cut never splits a UTF-8 sequence).
std::string clip(std::string_view name)
{
    std::size_t chars = 0, i = 0;
    for (; i < name.size(); ++i) {
        if ((static_cast<unsigned char>(name[i]) & 0xC0) == 0x80) continue;   // a continuation byte belongs to the character before it
        if (chars == kMaxEchoedChars) break;
        ++chars;
    }
    return std::string{name.substr(0, i)};
}

/// Why a request whose root is not an object is refused: it is valid JSON of another type, or not JSON at all. (The type is read from the first byte, so
/// `not json` looks like `null` until the token is checked.)
Err notAnObject(simdjson::ondemand::document& doc)
{
    const Err invalid{"bad-request", "", "the request is not valid JSON"};
    const std::string_view raw = json::trimRaw(doc.raw_json().value());
    if (raw == "null" || raw == "true" || raw == "false" || raw.starts_with('[') || raw.starts_with('"') || raw.starts_with('-') || (!raw.empty() && raw.front() >= '0' && raw.front() <= '9')) {
        if (!doc.at_end()) return invalid;
        return Err{"bad-request", "", "the request is not a JSON object"};
    }
    return invalid;
}

std::expected<Parsed, Err> parseRequest(std::string_view text)
{
    Parsed p;
    try {
        json::ParseSession session;
        auto doc = session.iterate(text);
        if (doc.type().value() != simdjson::ondemand::json_type::object) return std::unexpected(notAnObject(doc));
        simdjson::ondemand::object obj = doc.get_object();
        for (auto field : obj) {
            const std::string_view key = field.unescaped_key();
            auto value = field.value();
            p.names.emplace_back(key);
            const simdjson::ondemand::json_type type = value.type().value();
            if (key == "schema") {
                if (type == simdjson::ondemand::json_type::string) { std::string_view s = value.get_string(); p.schema = std::string{s}; p.schemaBad = false; }
                else p.schemaBad = true;
            } else if (key == "op") {
                if (type == simdjson::ondemand::json_type::string) { std::string_view s = value.get_string(); p.op = std::string{s}; p.opBad = false; }
                else p.opBad = true;
            } else if (key == "id") {
                bool ok = type == simdjson::ondemand::json_type::string;
                if (type == simdjson::ondemand::json_type::number) {
                    const simdjson::ondemand::number_type nt = value.get_number_type().value();
                    ok = nt == simdjson::ondemand::number_type::unsigned_integer || nt == simdjson::ondemand::number_type::signed_integer;
                }
                if (ok) { p.idRaw = std::string{json::trimRaw(value.raw_json().value())}; p.idBad = false; }
                else { p.idRaw.reset(); p.idBad = true; }
            } else if (key == "limit") {
                std::uint64_t n = 0;
                if (type == simdjson::ondemand::json_type::number && !value.get_uint64().get(n) && n > 0) { p.limit = n; p.limitBad = false; }
                else { p.limit.reset(); p.limitBad = true; }
            } else if (key == "cursor") {
                if (type == simdjson::ondemand::json_type::null) { p.cursorRaw.reset(); p.cursorBad = false; }
                else if (type == simdjson::ondemand::json_type::object) { p.cursorRaw = std::string{json::trimRaw(value.raw_json().value())}; p.cursorBad = false; }
                else { p.cursorRaw.reset(); p.cursorBad = true; }
            } else if (key == "order") {
                if (type == simdjson::ondemand::json_type::string) { std::string_view s = value.get_string(); p.order = std::string{s}; p.orderBad = false; }
                else p.orderBad = true;
            }
        }
        if (!doc.at_end()) return std::unexpected(Err{"bad-request", "", "the request is not valid JSON"});
    } catch (const simdjson::simdjson_error&) {
        return std::unexpected(Err{"bad-request", "", "the request is not valid JSON"});
    }
    return p;
}

/// Whether `name` is a member `op` takes.
bool takes(std::string_view op, std::string_view name)
{
    if (name == "schema" || name == "op" || name == "id") return true;
    if (op == "streams") return name == "limit" || name == "cursor";
    if (op == "since") return name == "limit" || name == "cursor" || name == "order";
    return false;
}

struct Limit
{
    std::uint32_t size;
    bool clamped;
};

Limit effectiveLimit(const std::optional<std::uint64_t>& asked, ProtocolPolicy::Page page)
{
    if (!asked) return {page.defaultSize, false};
    if (*asked > page.maxSize) return {page.maxSize, true};
    return {static_cast<std::uint32_t>(*asked), false};
}

/// A `streams` cursor: the place `{"namespace":..,"writer":..}` of the last writer a page sent.
std::expected<WriterId, Err> parsePlace(std::string_view raw)
{
    WriterId id;
    bool haveNs = false, haveWriter = false;
    try {
        json::ParseSession session;
        auto doc = session.iterate(raw);
        for (auto field : doc.get_object()) {
            const std::string_view key = field.unescaped_key();
            auto value = field.value();
            if (key == "namespace") { std::string_view s = value.get_string(); id.ns.assign(s); haveNs = true; }
            else if (key == "writer") { std::string_view s = value.get_string(); id.writer.assign(s); haveWriter = true; }
        }
    } catch (const simdjson::simdjson_error&) {
        return std::unexpected(Err{"bad-cursor", "cursor", "the cursor is not a place {\"namespace\":..,\"writer\":..}"});
    }
    if (!haveNs || !haveWriter) return std::unexpected(Err{"bad-cursor", "cursor", "the cursor is not a place {\"namespace\":..,\"writer\":..}"});
    return id;
}

void appendPlace(json::Writer& w, const WriterId& id) { w.beginObject().kv("namespace", id.ns).kv("writer", id.writer).endObject(); }

Payload opInfo(const ProtocolPolicy& policy)
{
    json::Writer p;
    p.beginObject().key("ops").beginArray();
    for (std::string_view op : kOps) p.str(op);
    p.endArray().key("policy").raw(protocolPolicyJson(policy));
    p.key("shapes").beginArray();
    for (const RecordCodec* c : codecs()) p.str(c->shape());
    p.endArray().key("orders").beginArray();
    for (const OrderPolicy& o : orderPolicies()) p.str(o.name);
    p.endArray().endObject();
    return {membersOf(p), 0, 0};
}

std::expected<Payload, Err> opStreams(const Parsed& request, const std::vector<StreamSummary>& list, const ProtocolPolicy& policy)
{
    std::size_t index = 0;
    if (request.cursorRaw) {
        const auto place = parsePlace(*request.cursorRaw);
        if (!place) return std::unexpected(place.error());
        // A cursor is a place: the page starts after the last writer sent, wherever the list has grown or shrunk since.
        index = static_cast<std::size_t>(std::ranges::upper_bound(list, *place, std::less<>{}, &StreamSummary::writer) - list.begin());
    }
    const Limit limit = effectiveLimit(request.limit, policy.streams);
    std::string entries;
    std::uint64_t sent = 0;
    const WriterId* last = nullptr;
    while (index < list.size() && sent < limit.size && !(sent > 0 && entries.size() >= policy.maxResponseBytes)) {
        const StreamSummary& s = list[index++];
        json::Writer e;
        e.beginObject().kv("namespace", s.writer.ns).kv("writer", s.writer.writer).kv("shape", s.shape).kv("order", s.order).kv("records", s.records);
        if (!s.damage.empty()) e.kv("damage", s.damage);
        e.endObject();
        if (sent > 0) entries += ',';
        entries.append(e.view());
        last = &s.writer;
        ++sent;
    }
    json::Writer p;
    p.beginObject().key("entries").raw("[" + entries + "]").key("next");
    if (index < list.size() && last != nullptr) appendPlace(p, *last);
    else p.raw("null");
    if (limit.clamped) p.kvRaw("clamped", "true");
    p.endObject();
    return Payload{membersOf(p), 0, sent};
}

std::expected<Payload, Err> opSince(const Parsed& request, const QuerySource& source, const std::vector<StreamSummary>& list, const ProtocolPolicy& policy)
{
    Cursor cursor;
    if (request.cursorRaw) {
        auto parsed = Cursor::fromJson(*request.cursorRaw);
        if (!parsed) return std::unexpected(Err{"bad-cursor", "cursor", "the cursor is not a cursor-1 object"});
        cursor = std::move(*parsed);
    }
    std::string_view order;
    if (request.order) {
        order = *request.order;
        if (findOrderPolicy(order) == nullptr) return std::unexpected(Err{"bad-request", "order", std::format("unknown order policy '{}'", order)});
    }
    const auto resolved = resolveReadOrder(list, order);
    if (!resolved) return std::unexpected(Err{"mixed-order", "order", resolved.error()});

    std::map<WriterId, const RecordCodec*> codecOf;   // each record is written in its stream's shape; the stream is known by its writer
    for (const StreamSummary& s : list) {
        const RecordCodec* codec = findCodec(s.shape);
        if (codec == nullptr) return std::unexpected(Err{"store-error", "", std::format("stream '{}/{}' has the unknown shape '{}'", s.writer.ns, s.writer.writer, s.shape)});
        codecOf.emplace(s.writer, codec);
    }
    auto stream = source.since(cursor, order);
    if (!stream) return std::unexpected(Err{"store-error", "", stream.error()});

    const Limit limit = effectiveLimit(request.limit, policy.since);
    std::string entries;
    std::uint64_t sent = 0;
    bool more = false;
    Record record;
    for (;;) {
        // The page is full at its limit, or once its entries reach the byte cap (the first entry is always added): one more pull says whether there is more.
        const bool full = sent >= limit.size || (sent > 0 && entries.size() >= policy.maxResponseBytes);
        const auto got = (*stream)->next(record);   // a pull and the cursor advance below are separate statements: the order they happen in is the contract
        if (!got) {
            // Damage reached: records before it still go out; a page that has none says so, so the client sees the damage rather than a silent end.
            if (sent == 0) return std::unexpected(Err{"store-error", "", got.error()});
            more = true;
            break;
        }
        if (!*got) break;
        if (full) { more = true; break; }
        const auto codec = codecOf.find(record.writer);
        if (codec == codecOf.end())
            return std::unexpected(Err{"store-error", "", std::format("a record of '{}/{}' came from a stream the listing does not have", record.writer.ns, record.writer.writer)});
        std::string wire;
        codec->second->encode(record, wire);
        json::Writer e;
        e.beginObject().key("writer");
        appendPlace(e, record.writer);
        e.kv("seq", record.seq).kv("shape", codec->second->shape()).kvRaw("record", wire).endObject();
        if (sent > 0) entries += ',';
        entries.append(e.view());
        cursor.advance(record);
        ++sent;
    }
    json::Writer p;
    p.beginObject().key("entries").raw("[" + entries + "]").key("next").raw(cursor.toJson()).kvRaw("more", more ? "true" : "false");
    if (limit.clamped) p.kvRaw("clamped", "true");
    p.endObject();
    return Payload{membersOf(p), sent, 0};
}

}  // namespace

ProtocolServer::ProtocolServer(const QuerySource& source, ProtocolPolicy policy) : source_(&source), policy_(policy) {}

std::string ProtocolServer::handle(std::string_view request, const RequestContext& context) { return handle(request, context, nullptr); }

std::string ProtocolServer::handle(std::string_view request, const RequestContext& context, ProtocolStats* stats)
{
    (void)context;   // W3a answers every caller alike; identity is the transport's, and W3b's projecting handler is the one that reads it (contract P-7)
    Echo echo;
    Payload payload;
    std::string response;
    const auto fail = [&](Err e) { response = errorJson(echo, e); };

    [&] {
        if (request.size() > policy_.maxRequestBytes) {   // before the parse: a request over the cap costs nothing to refuse
            fail({"too-large", "", std::format("the request is {} bytes; the limit is {}", request.size(), policy_.maxRequestBytes)});
            return;
        }
        auto parsed = parseRequest(request);
        if (!parsed) { fail(parsed.error()); return; }
        const Parsed& p = *parsed;
        if (p.op) echo.op = p.op;
        if (p.idRaw) echo.idRaw = p.idRaw;
        if (p.idBad) return fail({"bad-request", "id", "id must be a string or a whole number"});
        if (p.schemaBad) return fail({"bad-request", "schema", "schema must be a string"});
        if (p.schema && *p.schema != kProtocolSchema)
            return fail({"unsupported-schema", "schema", std::format("this server speaks '{}', not '{}'", kProtocolSchema, *p.schema)});
        if (p.opBad) return fail({"bad-request", "op", "op must be a string"});
        if (!p.op) return fail({"bad-request", "op", "op is required"});
        const std::string& op = *p.op;
        if (std::ranges::find(kOps, op) == kOps.end()) return fail({"unknown-op", "op", std::format("unknown op '{}'", op)});

        // Malformed members of this op, then members it does not take (a malformed `limit` on `info` is an unknown member, not an error).
        if (takes(op, "limit") && p.limitBad) return fail({"bad-request", "limit", "limit must be a positive whole number"});
        if (takes(op, "cursor") && p.cursorBad) return fail({"bad-cursor", "cursor", "the cursor must be an object or null"});
        if (takes(op, "order") && p.orderBad) return fail({"bad-request", "order", "order must be a string"});
        std::vector<std::string> unknown;
        for (const std::string& name : p.names)
            if (!takes(op, name)) unknown.push_back(name);
        if (!unknown.empty() && policy_.unknownFields == UnknownFields::reject)
            return fail({"unknown-field", clip(unknown.front()), std::format("this request has a member that '{}' does not take", op)});

        const std::vector<StreamSummary> list = source_->streams();
        if (op == "info") {
            payload = opInfo(policy_);
        } else {
            auto answered = op == "streams" ? opStreams(p, list, policy_) : opSince(p, *source_, list, policy_);
            if (!answered) return fail(answered.error());
            payload = std::move(*answered);
        }

        std::uint64_t records = 0;
        for (const StreamSummary& s : list) records += s.records;
        const std::uint64_t streamCount = list.size();
        json::Writer w;
        w.beginObject().kv("schema", kProtocolSchema).kvRaw("ok", "true").kv("op", op);
        if (p.idRaw) w.kvRaw("id", *p.idRaw);
        w.key("at").beginObject().kv("streams", streamCount).kv("records", records).endObject();
        if (!payload.members.empty()) w.raw(payload.members);
        if (policy_.unknownFields == UnknownFields::echo && !unknown.empty()) {
            w.key("unknownFields").beginArray();
            for (std::size_t i = 0; i < unknown.size() && i < kMaxEchoedNames; ++i) w.str(clip(unknown[i]));
            w.endArray();
        }
        w.endObject();
        response.assign(w.view());
    }();

    if (stats != nullptr) {
        stats->recordsReturned += payload.records;
        stats->streamsListed += payload.streams;
        stats->responseBytes += response.size();
    }
    return response;
}

namespace {

/// A positive whole number that fits `max`, or the error naming the member.
std::expected<std::uint64_t, std::string> positive(simdjson::ondemand::value value, std::string_view name, std::uint64_t max)
{
    std::uint64_t n = 0;
    if (value.get_uint64().get(n) || n == 0 || n > max) return std::unexpected(std::format("{} must be a positive whole number{}", name, max < std::numeric_limits<std::uint64_t>::max() ? std::format(" of at most {}", max) : std::string{}));
    return n;
}

std::expected<void, std::string> parsePage(simdjson::ondemand::value value, std::string_view name, ProtocolPolicy::Page& page)
{
    simdjson::ondemand::object obj;
    if (value.get_object().get(obj)) return std::unexpected(std::format("{} must be an object {{\"default\":N,\"max\":N}}", name));
    for (auto field : obj) {
        const std::string_view key = field.unescaped_key();
        auto v = field.value();
        const std::string member = std::format("{}.{}", name, key);
        if (key != "default" && key != "max") return std::unexpected(std::format("unknown policy member '{}'", member));
        const auto n = positive(v.value(), member, std::numeric_limits<std::uint32_t>::max());
        if (!n) return std::unexpected(n.error());
        (key == "default" ? page.defaultSize : page.maxSize) = static_cast<std::uint32_t>(*n);
    }
    if (page.defaultSize > page.maxSize) return std::unexpected(std::format("{}.default ({}) is above {}.max ({})", name, page.defaultSize, name, page.maxSize));
    return {};
}

}  // namespace

std::expected<ProtocolPolicy, std::string> parseProtocolPolicy(std::string_view text)
{
    ProtocolPolicy p;
    try {
        json::ParseSession session;
        auto doc = session.iterate(text);
        if (doc.type().value() != simdjson::ondemand::json_type::object) return std::unexpected("the policy must be a JSON object");
        for (auto field : doc.get_object()) {
            const std::string_view key = field.unescaped_key();
            auto value = field.value();
            if (key == "streams" || key == "since") {
                auto ok = parsePage(value.value(), key, key == "streams" ? p.streams : p.since);
                if (!ok) return std::unexpected(ok.error());
            } else if (key == "maxResponseBytes" || key == "maxRequestBytes") {
                const auto n = positive(value.value(), key, std::numeric_limits<std::uint64_t>::max());
                if (!n) return std::unexpected(n.error());
                (key == "maxResponseBytes" ? p.maxResponseBytes : p.maxRequestBytes) = static_cast<std::size_t>(*n);
            } else if (key == "unknownFields") {
                std::string_view mode;
                if (value.get_string().get(mode)) return std::unexpected("unknownFields must be \"ignore\", \"echo\" or \"reject\"");
                if (mode == "ignore") p.unknownFields = UnknownFields::ignore;
                else if (mode == "echo") p.unknownFields = UnknownFields::echo;
                else if (mode == "reject") p.unknownFields = UnknownFields::reject;
                else return std::unexpected("unknownFields must be \"ignore\", \"echo\" or \"reject\"");
            } else {
                return std::unexpected(std::format("unknown policy member '{}'", key));
            }
        }
    } catch (const simdjson::simdjson_error& e) {
        return std::unexpected(std::format("the policy is not valid JSON: {}", e.what()));
    }
    // A member given alone may leave a default above a maximum it did not know about: check both pages whole.
    if (p.streams.defaultSize > p.streams.maxSize) return std::unexpected(std::format("streams.default ({}) is above streams.max ({})", p.streams.defaultSize, p.streams.maxSize));
    if (p.since.defaultSize > p.since.maxSize) return std::unexpected(std::format("since.default ({}) is above since.max ({})", p.since.defaultSize, p.since.maxSize));
    return p;
}

std::string protocolPolicyJson(const ProtocolPolicy& policy)
{
    const auto page = [](json::Writer& w, std::string_view name, ProtocolPolicy::Page p) {
        w.key(name).beginObject().kv("default", static_cast<std::uint64_t>(p.defaultSize)).kv("max", static_cast<std::uint64_t>(p.maxSize)).endObject();
    };
    json::Writer w;
    w.beginObject();
    page(w, "streams", policy.streams);
    page(w, "since", policy.since);
    w.kv("maxResponseBytes", static_cast<std::uint64_t>(policy.maxResponseBytes)).kv("maxRequestBytes", static_cast<std::uint64_t>(policy.maxRequestBytes));
    const char* mode = policy.unknownFields == UnknownFields::ignore ? "ignore" : policy.unknownFields == UnknownFields::reject ? "reject" : "echo";
    w.kv("unknownFields", std::string_view{mode}).endObject();
    return std::string{w.view()};
}

}  // namespace sub0fabric
