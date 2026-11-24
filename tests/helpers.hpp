#pragma once
// Shared by the tests: small record builders and byte helpers. Deterministic: no clock, no random.
#include "sub0fabric/pure/codec.hpp"
#include "sub0fabric/pure/record.hpp"
#include "sub0fabric/pure/segment.hpp"

#include <string>
#include <vector>

namespace fabrictest {

inline sub0fabric::Record canvasRecord(std::string ns, std::string writer, std::uint64_t lamport, std::string kind = "item.put@1", std::string payload = "{}")
{
    sub0fabric::Record r;
    r.kind = std::move(kind);
    r.writer = {std::move(ns), std::move(writer)};
    r.lamport = lamport;
    r.subject = "k" + std::to_string(lamport);
    r.time = "2026-11-24T00:00:00Z";
    r.payload = std::move(payload);
    return r;
}

inline sub0fabric::Record journalRecord(std::string type, std::string body, double ts = 1766000000.5)
{
    sub0fabric::Record r;
    r.kind = std::move(type);
    r.payload = std::move(body);
    r.ts = ts;
    return r;
}

/// The bytes of a whole frames segment: a header and one frame per record, `seq` assigned 1..n.
inline std::string frameImage(const sub0fabric::SegmentHeader& h, std::vector<sub0fabric::Record> records)
{
    const sub0fabric::RecordCodec* codec = sub0fabric::findCodec(h.shape);
    std::string out = sub0fabric::encodeHeader(h);
    std::uint64_t seq = h.first;
    for (auto& r : records) {
        r.seq = ++seq;
        (void)sub0fabric::appendRecord(out, sub0fabric::Framing::frames, *codec, r);
    }
    return out;
}

/// The bytes of a whole lines segment.
inline std::string lineImage(const sub0fabric::RecordCodec& codec, std::vector<sub0fabric::Record> records)
{
    std::string out;
    std::uint64_t seq = 0;
    for (auto& r : records) {
        r.seq = ++seq;
        (void)sub0fabric::appendRecord(out, sub0fabric::Framing::lines, codec, r);
    }
    return out;
}

}  // namespace fabrictest
