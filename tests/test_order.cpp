// Order policies and the streaming merge (order.hpp), cursors (cursor.hpp), the sparse index (index.hpp) and on-disk names (layout.hpp).
#include "helpers.hpp"
#include "sub0fabric/pure/cursor.hpp"
#include "sub0fabric/pure/index.hpp"
#include "sub0fabric/pure/layout.hpp"
#include "sub0fabric/pure/order.hpp"
#include "sub0platform/testing/check.hpp"

#include <algorithm>

using namespace sub0fabric;
using namespace fabrictest;

namespace {

/// One writer's records with the given lamport values, seq 1..n.
std::vector<Record> stream(const std::string& ns, const std::string& writer, std::vector<std::uint64_t> lamports)
{
    std::vector<Record> v;
    std::uint64_t seq = 0;
    for (auto l : lamports) {
        Record r = canvasRecord(ns, writer, l);
        r.seq = ++seq;
        v.push_back(std::move(r));
    }
    return v;
}

std::vector<std::unique_ptr<RecordSource>> sources(std::vector<std::vector<Record>> all)
{
    std::vector<std::unique_ptr<RecordSource>> out;
    for (auto& s : all) out.push_back(std::make_unique<MemorySource>(std::move(s)));
    return out;
}

std::vector<Record> drainAll(Merger& m)
{
    std::vector<Record> out;
    Record r;
    for (;;) {
        auto got = m.next(r);
        CHECK(got.has_value());
        if (!got || !*got) break;
        out.push_back(r);
    }
    return out;
}

struct Failing final : RecordSource
{
    int n = 0;
    std::expected<bool, std::string> next(Record& out) override
    {
        if (n++ < 2) { out = canvasRecord("", "f", static_cast<std::uint64_t>(n)); return true; }
        return std::unexpected("damaged");
    }
};

}  // namespace

int main()
{
    // Policies are data: found by name, unknown names are null.
    CHECK(findOrderPolicy("seq") != nullptr && findOrderPolicy("lamport") != nullptr && findOrderPolicy("aligned-time") == nullptr);
    CHECK_EQ(orderPolicies().size(), 2u);
    const OrderPolicy& lamport = *findOrderPolicy("lamport");
    const OrderPolicy& seq = *findOrderPolicy("seq");

    // Lamport merge: (lamport, writer), ties broken by writer then source, equal to a sort of everything.
    {
        auto a = stream("p", "a", {1, 4, 6, 9});
        auto b = stream("p", "b", {2, 4, 5, 10});
        auto c = stream("q", "a", {3, 4, 7});
        std::vector<Record> expected;
        for (auto* v : {&a, &b, &c}) expected.insert(expected.end(), v->begin(), v->end());
        std::ranges::sort(expected, [&](const Record& x, const Record& y) { return lamport.less(x, y); });
        Merger m(sources({a, b, c}), lamport);
        auto got = drainAll(m);
        CHECK(got == expected);
        CHECK_EQ(m.outOfOrder(), 0u);
        // The three records with lamport 4 come out in writer order: (p,a), (p,b), (q,a).
        std::vector<std::string> fours;
        for (const Record& r : got) if (r.lamport == 4) fours.push_back(r.writer.ns + "/" + r.writer.writer);
        CHECK(fours == (std::vector<std::string>{"p/a", "p/b", "q/a"}));
        // The source order does not change the output.
        Merger m2(sources({c, b, a}), lamport);
        CHECK(drainAll(m2) == expected);
    }

    // Seq policy: position first, then writer.
    {
        auto a = stream("p", "a", {10, 20, 30});
        auto b = stream("p", "b", {1, 2});
        Merger m(sources({a, b}), seq);
        auto got = drainAll(m);
        CHECK_EQ(got.size(), 5u);
        CHECK(got[0].writer.writer == "a" && got[0].seq == 1 && got[1].writer.writer == "b" && got[1].seq == 1);
        CHECK(got[4].writer.writer == "a" && got[4].seq == 3);
    }

    // Edge cases: no sources, empty sources, one source (passes through), a source that violates its order (counted, merge continues).
    {
        Merger none(sources({}), lamport);
        Record r;
        auto e = none.next(r);
        CHECK(e.has_value() && !*e);
        Merger empties(sources({{}, {}}), lamport);
        auto e2 = empties.next(r);
        CHECK(e2.has_value() && !*e2);
        auto one = stream("p", "a", {1, 2, 3});
        Merger single(sources({one}), lamport);
        CHECK(drainAll(single) == one);
        auto bad = stream("p", "a", {5, 3, 8});
        Merger violating(sources({bad, stream("p", "b", {4, 6})}), lamport);
        CHECK_EQ(drainAll(violating).size(), 5u);
        CHECK_EQ(violating.outOfOrder(), 1u);
    }

    // A source error stops the merge and is reported, never swallowed.
    {
        std::vector<std::unique_ptr<RecordSource>> v;
        v.push_back(std::make_unique<Failing>());
        v.push_back(std::make_unique<MemorySource>(stream("p", "a", {100, 200, 300})));
        Merger m(std::move(v), lamport);
        Record r;
        bool sawError = false;
        for (int i = 0; i < 10 && !sawError; ++i) {
            auto got = m.next(r);
            if (!got) { sawError = true; CHECK_EQ(got.error(), std::string{"damaged"}); }
            else if (!*got) break;
        }
        CHECK(sawError);
    }

    // Cursor: per-writer positions, monotone advance, durable text round trip (equal cursors give equal bytes).
    {
        Cursor c;
        CHECK(c.empty());
        CHECK_EQ(c.position({"p", "a"}), 0u);
        Record r = canvasRecord("p", "a", 1);
        r.seq = 5;
        c.advance(r);
        r.seq = 3;
        c.advance(r);   // behind: no effect
        CHECK_EQ(c.position({"p", "a"}), 5u);
        c.set({"q", "weird \"id\"\n"}, 12);
        c.set({"", ""}, 1);
        auto text = c.toJson();
        auto back = Cursor::fromJson(text);
        CHECK(back.has_value() && *back == c);
        CHECK_EQ(back->toJson(), text);
        c.set({"", ""}, 0);
        CHECK_EQ(c.positions().size(), 2u);
        CHECK(!Cursor::fromJson("{}").has_value());
        CHECK(!Cursor::fromJson(R"({"format":"cursor-9","positions":[]})").has_value());
        CHECK(!Cursor::fromJson("nope").has_value());
        CHECK(Cursor::fromJson(R"({"format":"cursor-1","positions":[]})")->empty());
    }

    // Sparse index: one entry per `every` records; lookup gives the last entry at or before the position.
    {
        SparseIndex idx(4);
        for (std::uint64_t p = 10; p < 30; ++p) idx.note(p, p * 100);
        CHECK_EQ(idx.size(), 5u);   // 10, 14, 18, 22, 26
        CHECK_EQ(idx.lookup(10).position, 10u);
        CHECK_EQ(idx.lookup(13).position, 10u);
        CHECK_EQ(idx.lookup(14).offset, 1400u);
        CHECK_EQ(idx.lookup(29).position, 26u);
        CHECK_EQ(idx.lookup(3).position, 10u);   // before the first entry: the first
        CHECK(SparseIndex(0).every() == 1);
    }

    // Names: reversible, safe, distinct.
    {
        for (std::string_view id : {"", "w-000001", "a/b", "..", ".", ".hidden", "sp ace", "100%", "caf\xC3\xA9", "~", "a\\b:c*?\"<>|"}) {
            const std::string enc = encodeName(id);
            CHECK(enc != "." && enc != "..");
            CHECK(enc.find_first_of("/\\:*?\"<>| ") == std::string::npos);
            auto dec = decodeName(enc);
            CHECK(dec.has_value() && *dec == id);
        }
        CHECK(encodeName("~") != encodeName(""));
        CHECK(!decodeName("%4").has_value() && !decodeName("%ZZ").has_value());
        CHECK_EQ(segmentFileName(1), std::string{"seg-000001.s0f"});
        CHECK_EQ(segmentFileName(1234567), std::string{"seg-1234567.s0f"});
        CHECK(parseSegmentFileName("seg-000012.s0f") == 12u);
        CHECK(!parseSegmentFileName("seg-.s0f") && !parseSegmentFileName("seg-000000.s0f") && !parseSegmentFileName("journal-000001.ndjson") && !parseSegmentFileName("seg-1x.s0f"));
    }
    return finish();
}
