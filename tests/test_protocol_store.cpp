// The protocol over the directory store (io/store_query_source.hpp): the golden cases of tests/fixtures/fabric-query-1 answered from files written through
// StoreWriter must equal, byte for byte, what the in-memory source answers (test_protocol). Plus damage, and a read that follows an append.
#include "golden.hpp"
#include "helpers.hpp"
#include "sub0fabric/io/store.hpp"
#include "sub0fabric/io/store_query_source.hpp"
#include "sub0platform/testing/check.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

using namespace sub0fabric;
using namespace sub0fabric::io;
using namespace fabrictest;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

std::string ask(RequestHandler& server, std::string_view request) { return server.handle(request, RequestContext{}); }

/// Write a source set of the fixtures into `root`: canvas streams as frames, the journal as the lines file, one directory per writer.
void writeSet(const fs::path& root, const std::string& set)
{
    for (const golden::StreamData& s : golden::loadStreams(set)) {
        WriterOptions options;
        options.shape = s.shape;
        options.order = s.order;
        options.framing = s.shape == "journal-1" ? Framing::lines : Framing::frames;
        auto writer = StoreWriter::open(root, s.writer, options);
        CHECK(writer.has_value());
        if (!writer) continue;
        for (Record r : s.records) {
            const auto seq = writer->append(r);
            CHECK(seq.has_value());
        }
        CHECK(writer->sync().has_value());
    }
}

/// The (writer, seq) of every `since` entry in a response, in order (an entry starts `{"writer":{"namespace":"`, which no record starts with).
std::vector<std::pair<WriterId, std::uint64_t>> entriesOf(const std::string& response)
{
    std::vector<std::pair<WriterId, std::uint64_t>> out;
    const std::string marker = R"({"writer":{"namespace":")";
    for (std::size_t at = response.find(marker); at != std::string::npos; at = response.find(marker, at + 1)) {
        WriterId w;
        std::size_t i = at + marker.size();
        std::size_t e = response.find('"', i);
        w.ns = response.substr(i, e - i);
        i = e + std::string{R"(","writer":")"}.size();
        e = response.find('"', i);
        w.writer = response.substr(i, e - i);
        const std::size_t seq = response.find(R"("seq":)", e) + 6;
        out.emplace_back(std::move(w), std::strtoull(response.c_str() + seq, nullptr, 10));
    }
    return out;
}

/// The cursor object at the `next` member of a `since` response.
std::string nextOf(const std::string& response)
{
    const std::size_t at = response.rfind(R"("next":)") + 7;
    std::size_t depth = 0, i = at;
    for (; i < response.size(); ++i) {
        if (response[i] == '{') ++depth;
        else if (response[i] == '}' && --depth == 0) break;
    }
    return response.substr(at, i + 1 - at);
}

}  // namespace

int main()
{
    // The golden cases, byte for byte: one store per source set.
    {
        std::map<std::string, fs::path> roots;
        std::map<std::string, StoreReader> readers;
        for (const std::string set : {"canvas", "journal", "both"}) {
            roots[set] = makeTempDir(("pq-" + set).c_str());
            writeSet(roots[set], set);
            auto reader = StoreReader::open(roots[set]);
            CHECK(reader.has_value());
            if (reader) readers.emplace(set, std::move(*reader));
        }
        const std::vector<golden::Case> cases = golden::loadCases(golden::readFile(golden::fixtureDir() + "cases.txt"));
        CHECK_GE(cases.size(), 20u);
        for (const golden::Case& c : cases) {
            const auto reader = readers.find(c.source);
            CHECK(reader != readers.end());
            if (reader == readers.end()) continue;
            const StoreQuerySource source(reader->second);
            ProtocolServer server(source, golden::policyOf(c));
            const std::string got = ask(server, c.request);
            if (got != c.expected) std::fprintf(stderr, "cases.txt:%zu: %s\n  expected %s\n  got      %s\n", c.line, c.request.c_str(), c.expected.c_str(), got.c_str());
            CHECK_EQ(got, c.expected);
        }
        for (auto& [set, root] : roots) removeTree(root);
    }

    // A damaged segment: the listing says so, the records before the damage are delivered once, and then the read is a store-error that names the stream.
    {
        const fs::path root = makeTempDir("pq-damage");
        writeSet(root, "canvas");
        const fs::path segment = root / "proj" / "w-a" / "seg-000001.s0f";
        CHECK(fs::exists(segment));
        std::string bytes;
        {
            std::ifstream in(segment, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), {});
        }
        bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x55);   // inside a frame that is not the last: corrupt, not a torn tail
        {
            std::ofstream out(segment, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        auto reader = StoreReader::open(root);
        CHECK(reader.has_value());
        const StoreQuerySource source(*reader);
        ProtocolServer server(source);
        const std::string listing = ask(server, R"({"op":"streams"})");
        CHECK(contains(listing, R"("damage":")"));
        CHECK(contains(listing, "w-a"));

        std::string cursor;
        std::string last;
        std::set<std::pair<WriterId, std::uint64_t>> seen;
        for (int page = 0; page < 20; ++page) {
            std::string request = R"({"op":"since","limit":1)";
            if (!cursor.empty()) request += R"(,"cursor":)" + cursor;
            request += '}';
            last = ask(server, request);
            if (contains(last, R"("ok":false)")) break;
            for (const auto& e : entriesOf(last)) CHECK(seen.insert(e).second);   // never twice
            cursor = nextOf(last);
        }
        CHECK(contains(last, R"("ok":false,"op":"since","error":{"code":"store-error")"));
        CHECK(contains(last, "proj"));   // the message names the segment's path, which holds the stream's namespace and writer
        CHECK(contains(last, "w-a"));
        CHECK(!seen.empty() && seen.size() < 8);
        removeTree(root);
    }

    // An append, a refresh, and a read from the old `next` returns exactly the new records.
    {
        const fs::path root = makeTempDir("pq-append");
        writeSet(root, "canvas");
        auto reader = StoreReader::open(root);
        CHECK(reader.has_value());
        const StoreQuerySource source(*reader);
        ProtocolServer server(source);
        const std::string first = ask(server, R"({"op":"since"})");
        CHECK_EQ(entriesOf(first).size(), 8u);
        const std::string old = nextOf(first);

        {
            auto a = StoreWriter::open(root, {"proj", "w-a"});
            auto b = StoreWriter::open(root, {"proj", "w-b"});
            CHECK(a.has_value() && b.has_value());
            if (a && b) {
                Record r1 = canvasRecord("proj", "w-a", 9), r2 = canvasRecord("proj", "w-a", 11), r3 = canvasRecord("proj", "w-b", 10);
                CHECK(a->append(r1).has_value() && a->append(r2).has_value() && b->append(r3).has_value());
                CHECK(a->sync().has_value() && b->sync().has_value());
            }
        }
        // Not yet refreshed: the reader still holds what it scanned.
        CHECK(entriesOf(ask(server, R"({"op":"since","cursor":)" + old + "}")).empty());
        CHECK(reader->refresh().has_value());
        const std::string fresh = ask(server, R"({"op":"since","cursor":)" + old + "}");
        const auto got = entriesOf(fresh);
        using Seen = std::pair<WriterId, std::uint64_t>;
        CHECK(got == (std::vector<Seen>{{{"proj", "w-a"}, 5}, {{"proj", "w-b"}, 5}, {{"proj", "w-a"}, 6}}));   // lamport 9, 10, 11
        CHECK(contains(fresh, R"("at":{"streams":2,"records":11})"));
        CHECK(contains(fresh, R"("more":false)"));
        // And a further read from that next is empty, with the cursor unchanged.
        const std::string again = ask(server, R"({"op":"since","cursor":)" + nextOf(fresh) + "}");
        CHECK(entriesOf(again).empty());
        CHECK_EQ(nextOf(again), nextOf(fresh));
        removeTree(root);
    }
    return finish();
}
