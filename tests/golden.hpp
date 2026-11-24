#pragma once
// The golden contract suite of fabric-query-1 (tests/fixtures/fabric-query-1/): the records, and the request/response cases, shared by test_protocol (over
// the in-memory source) and test_protocol_store (over the directory store). Both must give the same bytes for every case.
#include "sub0fabric/pure/codec.hpp"
#include "sub0fabric/pure/protocol.hpp"
#include "sub0fabric/pure/query_source.hpp"
#include "sub0platform/testing/check.hpp"

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fabrictest::golden {

inline std::string fixtureDir() { return std::string{SUB0FABRIC_FIXTURE_DIR} + "/fabric-query-1/"; }

inline std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// The non-empty lines of `text`, with a trailing CR removed.
inline std::vector<std::string> linesOf(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

/// One stream of the fixtures: who wrote it, in what shape and order, and its records in position order.
struct StreamData
{
    sub0fabric::WriterId writer;
    std::string shape;
    std::string order;   ///< empty: the shape's default
    std::vector<sub0fabric::Record> records;
};

/// The streams of a source set: `canvas` (two writers, canvas-event-1), `journal` (one stream, journal-1) or `both`. The fixture lines are canonical: each
/// decodes and encodes back to the same bytes (checked here, so a hand-edit that is not canonical fails loudly).
inline std::vector<StreamData> loadStreams(const std::string& set)
{
    std::vector<StreamData> out;
    if (set == "canvas" || set == "both") {
        const sub0fabric::RecordCodec* codec = sub0fabric::findCodec("canvas-event-1");
        std::map<sub0fabric::WriterId, StreamData> byWriter;
        for (const std::string& line : linesOf(readFile(fixtureDir() + "canvas.ndjson"))) {
            if (line.empty()) continue;
            sub0fabric::Record r;
            const auto decoded = codec->decode(line, r);
            CHECK(decoded.has_value());
            std::string again;
            codec->encode(r, again);
            CHECK_EQ(again, line);
            StreamData& s = byWriter[r.writer];
            s.writer = r.writer;
            s.shape = "canvas-event-1";
            s.records.push_back(std::move(r));
        }
        for (auto& [writer, s] : byWriter) out.push_back(std::move(s));
    }
    if (set == "journal" || set == "both") {
        const sub0fabric::RecordCodec* codec = sub0fabric::findCodec("journal-1");
        StreamData s;
        s.writer = {"squad", "job-1"};
        s.shape = "journal-1";
        for (const std::string& line : linesOf(readFile(fixtureDir() + "journal.ndjson"))) {
            if (line.empty()) continue;
            sub0fabric::Record r;
            const auto decoded = codec->decode(line, r);
            CHECK(decoded.has_value());
            std::string again;
            codec->encode(r, again);
            CHECK_EQ(again, line);
            r.writer = s.writer;
            s.records.push_back(std::move(r));
        }
        out.push_back(std::move(s));
    }
    return out;
}

/// A memory source over a set.
inline sub0fabric::MemoryQuerySource memorySource(const std::string& set)
{
    sub0fabric::MemoryQuerySource src;
    for (StreamData& s : loadStreams(set)) CHECK(src.add(s.writer, s.shape, std::move(s.records), s.order).has_value());
    return src;
}

/// One case of cases.txt.
struct Case
{
    std::string source;       ///< canvas, journal or both
    std::string policy;       ///< the policy JSON in force (`{}` is the default)
    std::string request;
    std::string expected;
    std::size_t line = 0;     ///< the 1-based line of the `>>>` in the file, for messages
};

/// cases.txt: `# comment`, `@@@ source <set>`, `@@@ policy <json>` (both stay in force until changed), then `>>> request` followed by `<<< response`.
inline std::vector<Case> loadCases(const std::string& text)
{
    std::vector<Case> out;
    std::string source = "canvas", policy = "{}";
    const std::vector<std::string> lines = linesOf(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.starts_with("@@@ source ")) source = l.substr(11);
        else if (l.starts_with("@@@ policy ")) policy = l.substr(11);
        else if (l.starts_with(">>> ")) {
            Case c;
            c.source = source;
            c.policy = policy;
            c.request = l.substr(4);
            c.line = i + 1;
            if (i + 1 < lines.size() && lines[i + 1].starts_with("<<< ")) c.expected = lines[i + 1].substr(4);
            out.push_back(std::move(c));
        }
    }
    return out;
}

/// The policy a case runs under.
inline sub0fabric::ProtocolPolicy policyOf(const Case& c)
{
    const auto p = sub0fabric::parseProtocolPolicy(c.policy);
    CHECK(p.has_value());
    return p ? *p : sub0fabric::ProtocolPolicy{};
}

}  // namespace fabrictest::golden
