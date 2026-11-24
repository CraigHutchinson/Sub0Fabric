// The query protocol `fabric-query-1` (pure/protocol.hpp) over the in-memory query source (pure/query_source.hpp), one block per contract property of
// docs/DESIGN.md "Protocol (W3a)". The same cases run over the directory store in test_protocol_store.
#include "helpers.hpp"
#include "sub0fabric/pure/lines.hpp"
#include "sub0fabric/pure/protocol.hpp"
#include "sub0fabric/pure/query_source.hpp"
#include "sub0platform/testing/check.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>

using namespace sub0fabric;
using namespace fabrictest;

namespace {

/// The envelope's first members, as every success response starts.
const std::string kOk = R"({"schema":"fabric-query-1","ok":true,)";

/// Drain a source to the end.
std::vector<Record> drain(RecordSource& source)
{
    std::vector<Record> out;
    for (Record r;;) {
        auto got = source.next(r);
        CHECK(got.has_value());
        if (!got || !*got) break;
        out.push_back(r);
    }
    return out;
}

/// Canvas records for one writer with the given lamports.
std::vector<Record> canvasStream(const std::string& ns, const std::string& writer, std::vector<std::uint64_t> lamports)
{
    std::vector<Record> v;
    for (std::uint64_t l : lamports) v.push_back(canvasRecord(ns, writer, l));
    return v;
}

/// Three lamport writers whose records interleave.
void threeWriters(MemoryQuerySource& src)
{
    CHECK(src.add({"p", "a"}, "canvas-event-1", canvasStream("p", "a", {1, 4, 7, 10})).has_value());
    CHECK(src.add({"p", "b"}, "canvas-event-1", canvasStream("p", "b", {2, 5, 8})).has_value());
    CHECK(src.add({"p", "c"}, "canvas-event-1", canvasStream("p", "c", {3, 6, 9, 11, 12})).has_value());
}

std::string ask(RequestHandler& server, std::string_view request) { return server.handle(request, RequestContext{}); }

std::size_t count(const std::string& hay, std::string_view needle)
{
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

bool contains(const std::string& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

/// The `next` member of a list response as text: the object, or "" for null. The last `"next":` is the response's (entries come before it).
std::string nextOf(const std::string& response)
{
    const std::size_t at = response.rfind(R"("next":)");
    CHECK(at != std::string::npos);
    if (at == std::string::npos) return {};
    const std::size_t from = at + 7;
    if (response.compare(from, 4, "null") == 0) return {};
    std::size_t depth = 0, i = from;
    for (; i < response.size(); ++i) {
        if (response[i] == '{') ++depth;
        else if (response[i] == '}' && --depth == 0) break;
    }
    return response.substr(from, i + 1 - from);
}

bool moreOf(const std::string& response) { return contains(response, R"("more":true)"); }

struct Seen
{
    WriterId writer;
    std::uint64_t seq = 0;
    bool operator==(const Seen&) const = default;
};

/// The (writer, seq) of every `since` entry, in order. An entry starts `{"writer":{"namespace":"`, which no record starts with.
std::vector<Seen> entriesOf(const std::string& response)
{
    std::vector<Seen> out;
    const std::string marker = R"({"writer":{"namespace":")";
    for (std::size_t at = response.find(marker); at != std::string::npos; at = response.find(marker, at + 1)) {
        Seen s;
        std::size_t i = at + marker.size();
        std::size_t e = response.find('"', i);
        s.writer.ns = response.substr(i, e - i);
        i = e + std::string{R"(","writer":")"}.size();
        e = response.find('"', i);
        s.writer.writer = response.substr(i, e - i);
        const std::size_t seq = response.find(R"("seq":)", e) + 6;
        s.seq = std::strtoull(response.c_str() + seq, nullptr, 10);
        out.push_back(std::move(s));
    }
    return out;
}

struct Walk
{
    std::vector<Seen> seen;
    std::size_t pages = 0;
    std::string lastNext;
    std::string lastResponse;
};

/// Page through `since` with `limit` until `more` is false, each request carrying the previous `next`.
Walk walkSince(RequestHandler& server, std::size_t limit, const std::string& extra = {})
{
    Walk w;
    std::string cursor;
    for (; w.pages < 1000;) {
        std::string request = std::format(R"({{"op":"since","limit":{}{})", limit, extra);
        if (!cursor.empty()) request += R"(,"cursor":)" + cursor;
        request += '}';
        const std::string response = ask(server, request);
        CHECK(response.starts_with(kOk));
        ++w.pages;
        const std::vector<Seen> page = entriesOf(response);
        w.seen.insert(w.seen.end(), page.begin(), page.end());
        w.lastNext = nextOf(response);
        w.lastResponse = response;
        if (!moreOf(response)) break;
        cursor = w.lastNext;
    }
    return w;
}

/// A source that holds a damaged stream: the listing says so, and a read gives two records and then an error that names the file.
class DamagedSource final : public QuerySource
{
public:
    std::vector<StreamSummary> streams() const override { return {{{"p", "a"}, "canvas-event-1", "lamport", 3, "seg-000001.s0f: corrupt at byte 120"}}; }
    std::expected<std::unique_ptr<RecordSource>, std::string> since(const Cursor& cursor, std::string_view) const override
    {
        struct Failing final : RecordSource
        {
            std::uint64_t seq;
            std::expected<bool, std::string> next(Record& out) override
            {
                if (seq >= 2) return std::unexpected(std::string{"p/a: seg-000001.s0f: corrupt at byte 120"});
                out = canvasRecord("p", "a", seq + 1);
                out.seq = ++seq;
                return true;
            }
        };
        auto source = std::make_unique<Failing>();
        source->seq = cursor.position({"p", "a"});
        return source;
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------------------------

/// The query source seam, memory implementation: add sets seq, checks the shape, defaults the order, lists by writer.
void sourceSeam()
{
    MemoryQuerySource src;
    CHECK(src.add({"p", "b"}, "canvas-event-1", canvasStream("p", "b", {2, 4})).has_value());
    CHECK(src.add({"p", "a"}, "canvas-event-1", canvasStream("", "", {1, 3, 5})).has_value());   // the empty writer is stamped
    CHECK(!src.add({"p", "a"}, "canvas-event-1", {}).has_value());                              // twice
    CHECK(!src.add({"p", "c"}, "no-such-shape", {}).has_value());
    CHECK(!src.add({"p", "c"}, "canvas-event-1", {}, "no-such-order").has_value());
    CHECK(!src.add({"p", "c"}, "canvas-event-1", canvasStream("p", "other", {1})).has_value());   // a record of another writer
    const auto streams = src.streams();
    CHECK_EQ(streams.size(), 2u);
    CHECK(streams[0].writer == WriterId({"p", "a"}));
    CHECK_EQ(streams[0].records, 3u);
    CHECK_EQ(streams[0].shape, std::string{"canvas-event-1"});
    CHECK_EQ(streams[0].order, std::string{"lamport"});
    CHECK(streams[0].damage.empty());
    CHECK(streams[1].writer == WriterId({"p", "b"}));

    auto all = src.since(Cursor{}, {});
    CHECK(all.has_value());
    const auto recs = drain(**all);
    CHECK_EQ(recs.size(), 5u);
    for (std::size_t i = 0; i < recs.size(); ++i) CHECK_EQ(recs[i].lamport, i + 1);   // merged in (lamport, writer) order
    CHECK_EQ(recs[0].seq, 1u);
    CHECK_EQ(recs[4].seq, 3u);

    Cursor cursor;
    cursor.set({"p", "a"}, 2);
    cursor.set({"p", "b"}, 2);
    auto rest = src.since(cursor, {});
    CHECK(rest.has_value());
    const auto tail = drain(**rest);
    CHECK_EQ(tail.size(), 1u);   // only a's seq 3
    CHECK_EQ(tail[0].lamport, 5u);
    Cursor beyond;
    beyond.set({"p", "a"}, 99);   // beyond the end: nothing, not an error
    auto none = src.since(beyond, {});
    CHECK(none.has_value() && drain(**none).size() == 2u);   // b's two
}

/// Order resolution: a stream's own order, an override, mixed streams without an override (the store's error text), an unknown name.
void orderResolution()
{
    MemoryQuerySource src;
    CHECK(src.add({"p", "a"}, "canvas-event-1", canvasStream("p", "a", {1})).has_value());
    CHECK(src.add({"s", "j"}, "journal-1", {journalRecord("job.start", "{}")}).has_value());
    const auto streams = src.streams();
    CHECK_EQ(streams[1].order, std::string{"seq"});
    auto mixed = src.since(Cursor{}, {});
    CHECK(!mixed.has_value());
    CHECK_EQ(mixed.error(), std::string{"streams are read in different orders ('lamport' and 'seq'): name one"});
    auto named = src.since(Cursor{}, "seq");
    CHECK(named.has_value() && drain(**named).size() == 2u);
    auto unknown = src.since(Cursor{}, "aligned-time");
    CHECK(!unknown.has_value());
    CHECK_EQ(unknown.error(), std::string{"unknown order policy 'aligned-time'"});
    MemoryQuerySource empty;   // no streams: lamport, an empty read
    auto e = empty.since(Cursor{}, {});
    CHECK(e.has_value() && drain(**e).empty());
    MemoryQuerySource bySeq;   // an explicit order on a stream, as a segment header would say it
    CHECK(bySeq.add({"p", "a"}, "canvas-event-1", canvasStream("p", "a", {5, 6}), "seq").has_value());
    CHECK_EQ(bySeq.streams()[0].order, std::string{"seq"});
}

/// P-1: every failure is a structured error, with exact bytes.
void envelopeErrors()
{
    MemoryQuerySource src;
    threeWriters(src);
    ProtocolPolicy policy;
    policy.maxRequestBytes = 64;
    ProtocolServer server(src, policy);
    const auto error = [](std::string_view fields, std::string_view code, std::string_view field, std::string_view message) {
        return std::format(R"({{"schema":"fabric-query-1","ok":false{},"error":{{"code":"{}","field":"{}","message":"{}"}}}})", fields, code, field, message);
    };
    CHECK_EQ(ask(server, "not json"), error("", "bad-request", "", "the request is not valid JSON"));
    CHECK_EQ(ask(server, ""), error("", "bad-request", "", "the request is not valid JSON"));
    CHECK_EQ(ask(server, R"({"op":"info"} x)"), error("", "bad-request", "", "the request is not valid JSON"));
    CHECK_EQ(ask(server, R"({"op":"info")"), error("", "bad-request", "", "the request is not valid JSON"));
    CHECK_EQ(ask(server, "[1,2]"), error("", "bad-request", "", "the request is not a JSON object"));
    CHECK_EQ(ask(server, "7"), error("", "bad-request", "", "the request is not a JSON object"));
    CHECK_EQ(ask(server, "{}"), error("", "bad-request", "op", "op is required"));
    CHECK_EQ(ask(server, R"({"op":7})"), error("", "bad-request", "op", "op must be a string"));
    CHECK_EQ(ask(server, R"({"schema":"query-2","op":"info"})"),
             error(R"(,"op":"info")", "unsupported-schema", "schema", "this server speaks 'fabric-query-1', not 'query-2'"));
    CHECK_EQ(ask(server, R"({"schema":5,"op":"info"})"), error(R"(,"op":"info")", "bad-request", "schema", "schema must be a string"));
    CHECK_EQ(ask(server, R"({"op":"frobnicate","id":"r1"})"), error(R"(,"op":"frobnicate","id":"r1")", "unknown-op", "op", "unknown op 'frobnicate'"));
    CHECK_EQ(ask(server, R"({"op":"info","id":1.5})"), error(R"(,"op":"info")", "bad-request", "id", "id must be a string or a whole number"));
    CHECK_EQ(ask(server, R"({"op":"info","id":{"a":1}})"), error(R"(,"op":"info")", "bad-request", "id", "id must be a string or a whole number"));
    CHECK_EQ(ask(server, R"({"op":"streams","limit":0})"), error(R"(,"op":"streams")", "bad-request", "limit", "limit must be a positive whole number"));
    CHECK_EQ(ask(server, R"({"op":"streams","limit":-3})"), error(R"(,"op":"streams")", "bad-request", "limit", "limit must be a positive whole number"));
    CHECK_EQ(ask(server, R"({"op":"since","limit":"7"})"), error(R"(,"op":"since")", "bad-request", "limit", "limit must be a positive whole number"));
    CHECK_EQ(ask(server, R"({"op":"since","cursor":[1]})"), error(R"(,"op":"since")", "bad-cursor", "cursor", "the cursor must be an object or null"));
    CHECK_EQ(ask(server, R"({"op":"since","cursor":{"x":1}})"), error(R"(,"op":"since")", "bad-cursor", "cursor", "the cursor is not a cursor-1 object"));
    // A malformed member the op does not take is an unknown member, not an error.
    CHECK(ask(server, R"({"op":"info","limit":0})").starts_with(kOk));
    // The size is checked before the parse: a long request that is not even JSON is too-large, and never parsed.
    const std::string big(100, 'x');
    CHECK_EQ(ask(server, big), error("", "too-large", "", "the request is 100 bytes; the limit is 64"));
    const std::string atLimit = R"({"op":"info","pad":")" + std::string(64 - 22, 'x') + R"("})";
    CHECK_EQ(atLimit.size(), 64u);
    CHECK(ask(server, atLimit).starts_with(kOk));
}

/// The id is echoed as the raw token it came as.
void idEcho()
{
    MemoryQuerySource src;
    threeWriters(src);
    ProtocolServer server(src);
    for (const char* token : {R"("abc")", R"("quo\"te é é")", "42", "-3", "18446744073709551615", "0"}) {
        const std::string response = ask(server, std::format(R"({{"id":{},"op":"info"}})", token));
        CHECK(response.starts_with(kOk + R"("op":"info","id":)" + token + R"(,"at":)"));
        const std::string failure = ask(server, std::format(R"({{"id":{},"op":"nope"}})", token));
        CHECK(contains(failure, std::string{R"("op":"nope","id":)"} + token + R"(,"error")"));
    }
    const std::string none = ask(server, R"({"op":"info"})");
    CHECK(!contains(none, R"("id")"));
}

/// The three modes for members this version does not know.
void unknownFields()
{
    MemoryQuerySource src;
    threeWriters(src);
    const std::string known = R"({"op":"streams","limit":2})";
    {
        ProtocolPolicy policy;
        policy.unknownFields = UnknownFields::ignore;
        ProtocolServer server(src, policy);
        CHECK_EQ(ask(server, R"({"op":"streams","zzz":1,"limit":2,"yyy":{"a":[1]}})"), ask(server, known));   // as if absent, byte for byte
    }
    ProtocolServer echo(src);   // echo is the default
    const std::string r = ask(echo, R"({"op":"streams","b":1,"limit":2,"a":"x","c":{"d":[1,2]}})");
    CHECK(r.ends_with(R"(,"unknownFields":["b","a","c"]})"));
    CHECK(!contains(ask(echo, known), "unknownFields"));
    // A member the op does not take is unknown even when the server knows it elsewhere.
    CHECK(ask(echo, R"({"op":"info","limit":5,"order":"seq"})").ends_with(R"("unknownFields":["limit","order"]})"));
    CHECK(ask(echo, R"({"op":"streams","order":"seq"})").ends_with(R"("unknownFields":["order"]})"));
    // At most 16 names, each at most 64 characters (code points, never half of one).
    std::string many = R"({"op":"info")";
    for (int i = 0; i < 20; ++i) many += std::format(R"(,"f{:02}":0)", i);
    many += '}';
    const std::string manyResponse = ask(echo, many);
    CHECK(!contains(manyResponse, R"("f16")"));   // the seventeenth name is dropped
    CHECK(contains(manyResponse, R"("f15"])"));
    const std::string longName(100, 'n');
    CHECK(ask(echo, R"({"op":"info",")" + longName + R"(":1})").ends_with(R"("unknownFields":[")" + std::string(64, 'n') + R"("]})"));
    std::string accents;
    for (int i = 0; i < 70; ++i) accents += "\xC3\xA9";   // 70 x e-acute: 140 bytes, 70 characters
    CHECK(ask(echo, R"({"op":"info",")" + accents + R"(":1})").ends_with(R"("unknownFields":[")" + accents.substr(0, 128) + R"("]})"));
    // P-7: a member that names an actor is just an unknown member; the context is the transport's and the request cannot change it.
    RequestContext ctx;
    ctx.actor = {"p", "someone"};
    ctx.capabilities = {"read"};
    CHECK_EQ(echo.handle(R"({"op":"info","actor":{"namespace":"p","writer":"root"}})", ctx), ask(echo, R"({"op":"info","actor":{"namespace":"p","writer":"root"}})"));
    CHECK(ask(echo, R"({"op":"info","actor":"root"})").ends_with(R"("unknownFields":["actor"]})"));
    {
        ProtocolPolicy policy;
        policy.unknownFields = UnknownFields::reject;
        ProtocolServer server(src, policy);
        CHECK_EQ(ask(server, R"({"op":"info","second":1,"first":2,"id":4})"),
                 std::string{R"({"schema":"fabric-query-1","ok":false,"op":"info","id":4,"error":{"code":"unknown-field","field":"second","message":"this request has a member that 'info' does not take"}})"});
        CHECK(ask(server, R"({"op":"info","id":4})").starts_with(kOk));
    }
}

/// The policy is data: every member optional, each error names its member, and it round-trips.
void policyRoundtrip()
{
    const ProtocolPolicy defaults;
    const std::string text = protocolPolicyJson(defaults);
    CHECK_EQ(text, std::string{R"({"streams":{"default":100,"max":1000},"since":{"default":200,"max":2000},"maxResponseBytes":1048576,"maxRequestBytes":65536,"unknownFields":"echo"})"});
    const auto none = parseProtocolPolicy("{}");
    CHECK(none.has_value());
    if (none) CHECK_EQ(protocolPolicyJson(*none), text);
    const auto partial = parseProtocolPolicy(R"({"since":{"default":5},"unknownFields":"reject","maxResponseBytes":999})");
    CHECK(partial.has_value());
    if (partial) {
        CHECK_EQ(partial->since.defaultSize, 5u);
        CHECK_EQ(partial->since.maxSize, 2000u);
        CHECK_EQ(partial->maxResponseBytes, 999u);
        CHECK(partial->unknownFields == UnknownFields::reject);
        const std::string once = protocolPolicyJson(*partial);
        const auto again = parseProtocolPolicy(once);
        CHECK(again.has_value());
        if (again) CHECK_EQ(protocolPolicyJson(*again), once);
    }
    const auto ignore = parseProtocolPolicy(R"({"unknownFields":"ignore"})");
    CHECK(ignore.has_value() && ignore->unknownFields == UnknownFields::ignore);

    struct Bad
    {
        const char* text;
        const char* member;
    };
    for (const Bad& bad : {Bad{R"({"bogus":1})", "bogus"}, Bad{R"({"streams":{"default":0}})", "streams.default"}, Bad{R"({"streams":{"max":0}})", "streams.max"},
                           Bad{R"({"streams":{"default":10,"max":5}})", "streams.default"}, Bad{R"({"since":{"max":5}})", "since.default"},
                           Bad{R"({"since":{"default":1.5}})", "since.default"}, Bad{R"({"streams":{"zzz":1}})", "streams.zzz"},
                           Bad{R"({"streams":3})", "streams"}, Bad{R"({"maxResponseBytes":0})", "maxResponseBytes"},
                           Bad{R"({"maxRequestBytes":-1})", "maxRequestBytes"}, Bad{R"({"maxRequestBytes":"big"})", "maxRequestBytes"},
                           Bad{R"({"unknownFields":"loud"})", "unknownFields"}, Bad{R"({"unknownFields":3})", "unknownFields"},
                           Bad{R"({"streams":{"default":5000000000}})", "streams.default"}}) {
        const auto got = parseProtocolPolicy(bad.text);
        CHECK(!got.has_value());
        if (!got) {
            CHECK(contains(got.error(), bad.member));
        } else {
            std::fprintf(stderr, "policy accepted: %s\n", bad.text);
        }
    }
    CHECK(!parseProtocolPolicy("[]").has_value());
    CHECK(!parseProtocolPolicy("nope").has_value());

    MemoryQuerySource src;
    ProtocolServer server(src, *partial);
    CHECK(contains(ask(server, R"({"op":"info"})"), R"("policy":)" + protocolPolicyJson(*partial) + ","));
}

/// `info` says what this server speaks.
void info()
{
    MemoryQuerySource src;
    threeWriters(src);
    ProtocolServer server(src);
    CHECK_EQ(ask(server, R"({"op":"info","id":1})"),
             kOk + R"("op":"info","id":1,"at":{"streams":3,"records":12},"ops":["info","streams","since"],"policy":)" + protocolPolicyJson(ProtocolPolicy{}) +
                 R"(,"shapes":["journal-1","canvas-event-1"],"orders":["seq","lamport"]})");
    MemoryQuerySource empty;
    ProtocolServer none(empty);
    CHECK(contains(ask(none, R"({"op":"info"})"), R"("at":{"streams":0,"records":0})"));
}

/// P-6: a `streams` cursor is a place.
void streamsPaging()
{
    MemoryQuerySource src;
    for (const char* w : {"b", "d", "f", "h", "j"}) CHECK(src.add({"p", w}, "canvas-event-1", canvasStream("p", w, {1})).has_value());
    ProtocolPolicy policy;
    policy.streams = {2, 3};
    ProtocolServer server(src, policy);
    const auto writers = [](const std::string& whole) {
        const std::string r = whole.substr(0, whole.rfind(R"("next":)"));   // the entries, not the place
        std::string out;
        const std::string marker = R"({"namespace":"p","writer":")";
        for (std::size_t at = r.find(marker); at != std::string::npos; at = r.find(marker, at + 1)) out += r[at + marker.size()];
        return out;
    };

    const std::string first = ask(server, R"({"op":"streams"})");   // the default page: 2
    CHECK_EQ(first, kOk + R"("op":"streams","at":{"streams":5,"records":5},"entries":[{"namespace":"p","writer":"b","shape":"canvas-event-1","order":"lamport","records":1},)" +
                        R"({"namespace":"p","writer":"d","shape":"canvas-event-1","order":"lamport","records":1}],"next":{"namespace":"p","writer":"d"}})");
    // A writer added before the place neither repeats nor skips; one added after it appears in its place.
    CHECK(src.add({"p", "c"}, "canvas-event-1", canvasStream("p", "c", {1})).has_value());
    CHECK(src.add({"p", "e"}, "canvas-event-1", canvasStream("p", "e", {1})).has_value());
    const std::string second = ask(server, R"({"op":"streams","cursor":)" + nextOf(first) + "}");
    CHECK_EQ(writers(second), std::string{"ef"});
    const std::string third = ask(server, R"({"op":"streams","cursor":)" + nextOf(second) + "}");
    CHECK_EQ(writers(third), std::string{"hj"});
    CHECK(third.ends_with(R"("next":null})"));   // the last page
    CHECK(nextOf(third).empty());
    // Everything in one walk is the whole listing, ordered by writer.
    CHECK(!contains(first, "clamped"));
    const std::string big = ask(server, R"({"op":"streams","limit":99})");   // above the maximum of 3: clamped, and it says so
    CHECK_EQ(writers(big), std::string{"bcd"});
    CHECK(big.ends_with(R"("next":{"namespace":"p","writer":"d"},"clamped":true})"));
    const std::string exact = ask(server, R"({"op":"streams","limit":3})");
    CHECK(!contains(exact, "clamped"));
    const std::string all = ask(server, R"({"op":"streams","limit":3,"cursor":{"namespace":"p","writer":"d"}})");
    CHECK_EQ(writers(all), std::string{"efh"});
    CHECK(!nextOf(all).empty());   // j is left
    // A place beyond the end, and null as no cursor.
    CHECK(ask(server, R"({"op":"streams","cursor":{"namespace":"z","writer":"z"}})").ends_with(R"("entries":[],"next":null})"));
    CHECK_EQ(ask(server, R"({"op":"streams","cursor":null})"), ask(server, R"({"op":"streams"})"));
    // Bad cursors.
    for (const char* bad : {R"({"namespace":"p"})", R"({"namespace":1,"writer":"x"})", R"({})"}) {
        const std::string r = ask(server, std::string{R"({"op":"streams","cursor":)"} + bad + "}");
        CHECK(contains(r, R"("code":"bad-cursor","field":"cursor")"));
    }
    CHECK(contains(ask(server, R"({"op":"streams","cursor":"d"})"), R"("code":"bad-cursor")"));
    // No streams at all.
    MemoryQuerySource empty;
    ProtocolServer none(empty);
    CHECK(ask(none, R"({"op":"streams"})").ends_with(R"("entries":[],"next":null})"));
    // A damaged stream is listed with its damage.
    DamagedSource damaged;
    ProtocolServer d(damaged);
    CHECK(ask(d, R"({"op":"streams"})").ends_with(R"("records":3,"damage":"seg-000001.s0f: corrupt at byte 120"}],"next":null})"));
}

/// P-2: since pages give every record exactly once, per writer, in policy order.
void sinceExactlyOnce()
{
    MemoryQuerySource src;
    threeWriters(src);
    // The oracle: one full Merger drain.
    auto stream = src.since(Cursor{}, {});
    CHECK(stream.has_value());
    std::vector<Seen> oracle;
    Cursor full;
    for (const Record& r : drain(**stream)) {
        oracle.push_back({r.writer, r.seq});
        full.advance(r);
    }
    CHECK_EQ(oracle.size(), 12u);
    ProtocolServer server(src);
    for (std::size_t limit = 1; limit <= oracle.size() + 1; ++limit) {
        const Walk w = walkSince(server, limit);
        CHECK(w.seen == oracle);
        CHECK_EQ(w.pages, (oracle.size() + limit - 1) / limit);
        CHECK_EQ(w.lastNext, full.toJson());
        // A further read from the final cursor: nothing new, the same next.
        const std::string again = ask(server, R"({"op":"since","cursor":)" + w.lastNext + "}");
        CHECK(entriesOf(again).empty());
        CHECK_EQ(nextOf(again), w.lastNext);
        CHECK(again.ends_with(R"("entries":[],"next":)" + w.lastNext + R"(,"more":false})"));
    }
    // A request without a cursor starts at the beginning; the first page of 5 is the first 5 of the oracle.
    const std::string head = ask(server, R"({"op":"since","limit":5})");
    CHECK(moreOf(head));
    const auto firstFive = entriesOf(head);
    CHECK(firstFive == std::vector<Seen>(oracle.begin(), oracle.begin() + 5));
    // The limit is clamped to the policy maximum and says so.
    ProtocolPolicy small;
    small.since = {2, 4};
    ProtocolServer clamp(src, small);
    const std::string clamped = ask(clamp, R"({"op":"since","limit":50})");
    CHECK_EQ(entriesOf(clamped).size(), 4u);
    CHECK(clamped.ends_with(R"(,"more":true,"clamped":true})"));
    CHECK_EQ(entriesOf(ask(clamp, R"({"op":"since"})")).size(), 2u);   // the default
    // A damaged stream: the records before the damage go out, the page after them is the error, naming the file.
    DamagedSource damaged;
    ProtocolServer d(damaged);
    const std::string before = ask(d, R"({"op":"since"})");
    CHECK_EQ(entriesOf(before).size(), 2u);
    CHECK(moreOf(before));
    const std::string after = ask(d, R"({"op":"since","cursor":)" + nextOf(before) + "}");
    CHECK(contains(after, R"("code":"store-error")"));
    CHECK(contains(after, "p/a: seg-000001.s0f: corrupt at byte 120"));
}

/// P-2, the hard case: a writer that was slow, whose later record has a lower lamport than what was already read, still arrives on the next page.
void sinceLateWriter()
{
    MemoryQuerySource src;
    CHECK(src.add({"p", "a"}, "canvas-event-1", canvasStream("p", "a", {10, 20, 30})).has_value());
    ProtocolServer server(src);
    const std::string first = ask(server, R"({"op":"since"})");
    CHECK_EQ(entriesOf(first).size(), 3u);
    CHECK(!moreOf(first));
    CHECK(src.add({"p", "b"}, "canvas-event-1", canvasStream("p", "b", {12, 13})).has_value());   // below a's 30, already read
    const std::string second = ask(server, R"({"op":"since","cursor":)" + nextOf(first) + "}");
    const std::vector<Seen> got = entriesOf(second);
    CHECK_EQ(got.size(), 2u);
    CHECK(got == (std::vector<Seen>{{{"p", "b"}, 1}, {{"p", "b"}, 2}}));
    CHECK(nextOf(second) == R"({"format":"cursor-1","positions":[{"namespace":"p","writer":"a","position":3},{"namespace":"p","writer":"b","position":2}]})");
}

/// P-3: a page stops at its limit or at maxResponseBytes after its first entry.
void sinceBounds()
{
    MemoryQuerySource src;
    std::vector<Record> records;
    for (std::uint64_t l = 1; l <= 7; ++l) records.push_back(canvasRecord("p", "a", l, "item.put@1", R"({"pad":")" + std::string(1000, 'x') + R"("})"));
    CHECK(src.add({"p", "a"}, "canvas-event-1", records).has_value());
    ProtocolPolicy policy;
    policy.maxResponseBytes = 1500;   // an entry is a little over 1100 bytes: the second entry takes a page past the cap, so a page holds two
    ProtocolServer server(src, policy);
    const std::string first = ask(server, R"({"op":"since"})");
    CHECK_EQ(entriesOf(first).size(), 2u);
    CHECK(moreOf(first));
    const Walk w = walkSince(server, 100);
    CHECK_EQ(w.seen.size(), 7u);
    CHECK_EQ(w.pages, 4u);   // 2 + 2 + 2 + 1
    CHECK(!moreOf(w.lastResponse));
    for (std::size_t i = 0; i < w.seen.size(); ++i) CHECK_EQ(w.seen[i].seq, i + 1);

    // One entry larger than the cap goes out alone: the first entry is always added.
    MemoryQuerySource one;
    CHECK(one.add({"p", "a"}, "canvas-event-1",
                  {canvasRecord("p", "a", 1, "big@1", R"({"pad":")" + std::string(5000, 'y') + R"("})"), canvasRecord("p", "a", 2), canvasRecord("p", "a", 3)}).has_value());
    ProtocolPolicy tight;
    tight.maxResponseBytes = 1000;
    ProtocolServer s2(one, tight);
    const std::string alone = ask(s2, R"({"op":"since"})");
    CHECK_EQ(entriesOf(alone).size(), 1u);
    CHECK(alone.size() > 5000);
    CHECK(moreOf(alone));
    const std::string rest = ask(s2, R"({"op":"since","cursor":)" + nextOf(alone) + "}");
    CHECK_EQ(entriesOf(rest).size(), 2u);   // small entries: both fit under the cap
    CHECK(!moreOf(rest));
    // Alone and last: more is false.
    MemoryQuerySource last;
    CHECK(last.add({"p", "a"}, "canvas-event-1", {canvasRecord("p", "a", 1, "big@1", R"({"pad":")" + std::string(5000, 'y') + R"("})")}).has_value());
    ProtocolServer s3(last, tight);
    CHECK(!moreOf(ask(s3, R"({"op":"since"})")));
    // The streams listing is bounded the same way.
    MemoryQuerySource many;
    for (int i = 0; i < 30; ++i) CHECK(many.add({"p", std::format("w{:02}", i)}, "canvas-event-1", {}).has_value());
    ProtocolPolicy listPolicy;
    listPolicy.maxResponseBytes = 400;   // an entry is about 90 bytes
    ProtocolServer s4(many, listPolicy);
    const std::string page = ask(s4, R"({"op":"streams"})");
    CHECK_GE(count(page, R"("shape")"), 1u);
    CHECK(count(page, R"("shape")") < 10u);
    CHECK(!nextOf(page).empty());
}

/// Order: the streams' own, an override, mixed streams, unknown names.
void sinceOrder()
{
    MemoryQuerySource src;
    CHECK(src.add({"p", "a"}, "canvas-event-1", canvasStream("p", "a", {5, 6})).has_value());
    CHECK(src.add({"s", "j"}, "journal-1", {journalRecord("job.start", "{}"), journalRecord("job.end", "{}")}).has_value());
    ProtocolServer server(src);
    CHECK_EQ(ask(server, R"({"op":"since","id":9})"),
             std::string{R"({"schema":"fabric-query-1","ok":false,"op":"since","id":9,"error":{"code":"mixed-order","field":"order","message":"streams are read in different orders ('lamport' and 'seq'): name one"}})"});
    const std::string bySeq = ask(server, R"({"op":"since","order":"seq"})");
    CHECK(bySeq.starts_with(kOk));
    // (seq, writer): both writers' seq 1, the canvas writer first (p < s), then both seq 2.
    CHECK(entriesOf(bySeq) == (std::vector<Seen>{{{"p", "a"}, 1}, {{"s", "j"}, 1}, {{"p", "a"}, 2}, {{"s", "j"}, 2}}));
    const std::string byLamport = ask(server, R"({"op":"since","order":"lamport"})");
    CHECK(entriesOf(byLamport) == (std::vector<Seen>{{{"s", "j"}, 1}, {{"s", "j"}, 2}, {{"p", "a"}, 1}, {{"p", "a"}, 2}}));   // lamport 0 (journal) first
    CHECK_EQ(ask(server, R"({"op":"since","order":"aligned-time"})"),
             std::string{R"({"schema":"fabric-query-1","ok":false,"op":"since","error":{"code":"bad-request","field":"order","message":"unknown order policy 'aligned-time'"}})"});
    CHECK(contains(ask(server, R"({"op":"since","order":4})"), R"("code":"bad-request","field":"order")"));
    // Paging under an override is as exact as under the default.
    const Walk w = walkSince(server, 3, R"(,"order":"seq")");
    CHECK_EQ(w.seen.size(), 4u);
    CHECK(w.seen == entriesOf(bySeq));
}

/// P-4: the record is the codec's encode bytes, untouched; Canvas's unknown members and the payload survive; journal-1 carries its stream's writer.
void entriesVerbatim()
{
    MemoryQuerySource src;
    Record r = canvasRecord("p", "a", 7, "item.put@2", R"({"n":1e5,"s":"café","deep":{"a":[1,{"b":null}]}})");
    r.extra = R"("future":{"x":[1,2,3]},"flag":true)";
    Record plain = canvasRecord("p", "a", 9);
    CHECK(src.add({"p", "a"}, "canvas-event-1", {r, plain}).has_value());
    Record j = journalRecord("job.start", R"({"id":"j1","n":123456789012345})", 1766000000.5);
    CHECK(src.add({"squad", "job-1"}, "journal-1", {j}).has_value());
    ProtocolServer server(src);

    const RecordCodec* canvas = findCodec("canvas-event-1");
    const RecordCodec* journal = findCodec("journal-1");
    CHECK(canvas != nullptr && journal != nullptr);
    std::string first, second, third;
    r.seq = 1;
    plain.seq = 2;
    j.seq = 1;
    j.writer = {"squad", "job-1"};
    canvas->encode(r, first);
    canvas->encode(plain, second);
    journal->encode(j, third);
    CHECK(contains(first, R"("future":{"x":[1,2,3]},"flag":true})"));
    CHECK(contains(first, R"("n":1e5)"));

    const std::string canvasOnly = ask(server, R"({"op":"since","order":"lamport"})");   // journal's lamport is 0: it comes first
    CHECK_EQ(canvasOnly,
             kOk + R"("op":"since","at":{"streams":2,"records":3},"entries":[)" +
                 R"({"writer":{"namespace":"squad","writer":"job-1"},"seq":1,"shape":"journal-1","record":)" + third + "}," +
                 R"({"writer":{"namespace":"p","writer":"a"},"seq":1,"shape":"canvas-event-1","record":)" + first + "}," +
                 R"({"writer":{"namespace":"p","writer":"a"},"seq":2,"shape":"canvas-event-1","record":)" + second + R"(}],"next":)" +
                 nextOf(canvasOnly) + R"(,"more":false})");
    CHECK(contains(third, R"("ts":1766000000.5)"));
    // And decoding what came out gives the record back.
    Record back;
    CHECK(canvas->decode(first, back).has_value());
    CHECK(back.extra == r.extra && back.payload == r.payload);
}

/// The lines framing: one response line per request line.
void serveLinesBlock()
{
    MemoryQuerySource src;
    threeWriters(src);
    ProtocolPolicy policy;
    policy.maxRequestBytes = 200;
    ProtocolServer server(src, policy);
    const std::string input = "\n"
                              R"({"op":"info","id":1})" "\r\n"
                              "   \n"
                              R"({"op":"streams","id":2})" "\n"
                              "\r\n"
                              "garbage\n"
                              + std::string(500, 'z') + "\n"
                              R"({"op":"since","id":3,"limit":1})";   // no newline at the end
    const std::string out = serveLines(server, input, RequestContext{});
    CHECK(out.ends_with("\n"));
    CHECK_EQ(count(out, "\n"), 5u);   // five non-blank lines, five responses
    CHECK(!contains(out, "\r"));
    std::vector<std::string> lines;
    for (std::size_t at = 0; at < out.size();) {
        const std::size_t eol = out.find('\n', at);
        lines.push_back(out.substr(at, eol - at));
        at = eol + 1;
    }
    CHECK_EQ(lines.size(), 5u);
    if (lines.size() == 5) {
        CHECK(lines[0].starts_with(kOk + R"("op":"info","id":1,)"));
        CHECK(lines[1].starts_with(kOk + R"("op":"streams","id":2,)"));
        CHECK(contains(lines[2], R"("code":"bad-request")"));
        CHECK(contains(lines[3], R"("code":"too-large")"));
        CHECK(lines[4].starts_with(kOk + R"("op":"since","id":3,)"));
        CHECK_EQ(lines[1], ask(server, R"({"op":"streams","id":2})"));
    }
    CHECK(serveLines(server, "", RequestContext{}).empty());
    CHECK(serveLines(server, "\n\r\n  \n", RequestContext{}).empty());

    // A handler that does not behave: a raw newline in its answer is flattened, so the framing holds.
    struct Chatty final : RequestHandler
    {
        std::string handle(std::string_view, const RequestContext&) override { return "{\"a\":\n1,\r\n\"b\":2}"; }
    } chatty;
    CHECK_EQ(serveLines(chatty, "x\ny\n", RequestContext{}), std::string{"{\"a\": 1,  \"b\":2}\n{\"a\": 1,  \"b\":2}\n"});
}

/// P-5: same input, same bytes.
void determinism()
{
    MemoryQuerySource src;
    threeWriters(src);
    ProtocolServer one(src);
    ProtocolServer two(src);
    for (const char* request : {R"({"op":"info"})", R"({"op":"streams","limit":2})", R"({"op":"since","limit":5,"id":"x"})", R"({"op":"since","extra":1})", R"({"op":"bogus"})", "nope"}) {
        const std::string a = ask(one, request);
        CHECK_EQ(ask(one, request), a);
        CHECK_EQ(ask(two, request), a);
        CHECK(!contains(a, "\n"));
    }
    // A second source with the same records, built in another order, answers alike.
    MemoryQuerySource other;
    CHECK(other.add({"p", "c"}, "canvas-event-1", canvasStream("p", "c", {3, 6, 9, 11, 12})).has_value());
    CHECK(other.add({"p", "b"}, "canvas-event-1", canvasStream("p", "b", {2, 5, 8})).has_value());
    CHECK(other.add({"p", "a"}, "canvas-event-1", canvasStream("p", "a", {1, 4, 7, 10})).has_value());
    ProtocolServer three(other);
    CHECK_EQ(ask(three, R"({"op":"since","limit":7})"), ask(one, R"({"op":"since","limit":7})"));
    // Stats are added to, and count what a call did.
    ProtocolStats stats;
    const std::string r1 = one.handle(R"({"op":"since","limit":4})", RequestContext{}, &stats);
    const std::string r2 = one.handle(R"({"op":"streams"})", RequestContext{}, &stats);
    const std::string r3 = one.handle("nope", RequestContext{}, &stats);
    CHECK_EQ(stats.recordsReturned, 4u);
    CHECK_EQ(stats.streamsListed, 3u);
    CHECK_EQ(stats.responseBytes, r1.size() + r2.size() + r3.size());
}

}  // namespace

int main()
{
    sourceSeam();
    orderResolution();
    envelopeErrors();
    idEcho();
    unknownFields();
    policyRoundtrip();
    info();
    streamsPaging();
    sinceExactlyOnce();
    sinceLateWriter();
    sinceBounds();
    sinceOrder();
    entriesVerbatim();
    serveLinesBlock();
    determinism();
    return finish();
}
