// Record shapes (codec.hpp): journal-1 and canvas-event-1 as two proved shapes with disjoint field sets, their exact bytes, round trips and rejection of
// each other's text. Review D2 and the unverified claim of section 3 (do the two envelopes collide? they do not).
#include "helpers.hpp"
#include "sub0fabric/pure/codec.hpp"
#include "sub0platform/testing/check.hpp"

#include <fstream>

using namespace sub0fabric;

int main()
{
    const RecordCodec* journal = findCodec("journal-1");
    const RecordCodec* canvas = findCodec("canvas-event-1");
    CHECK(journal != nullptr && canvas != nullptr);
    CHECK(findCodec("nope") == nullptr);
    CHECK_EQ(codecs().size(), 2u);
    CHECK(fieldSetsDisjoint());
    if (journal == nullptr || canvas == nullptr) return finish();

    // journal-1: the exact bytes Sub0Squad's Journal writes (seq, ts, type, d).
    {
        Record r = fabrictest::journalRecord("a", R"({"x":1})", 1766000000.5);
        r.seq = 7;
        std::string line;
        journal->encode(r, line);
        CHECK_EQ(line, std::string{R"({"seq":7,"ts":1766000000.5,"type":"a","d":{"x":1}})"});
        Record back;
        CHECK(journal->decode(line, back).has_value());
        CHECK_EQ(back.seq, 7u);
        CHECK_EQ(back.kind, std::string{"a"});
        CHECK_EQ(back.payload, std::string{R"({"x":1})"});
        CHECK(back.ts == 1766000000.5);
        // A body with quotes and escapes is carried verbatim.
        Record e = fabrictest::journalRecord("b", R"({"s":"q\"uote\n"})");
        e.seq = 2;
        std::string eline;
        journal->encode(e, eline);
        Record eback;
        CHECK(journal->decode(eline, eback).has_value());
        CHECK_EQ(eback.payload, e.payload);
        // Unknown members are ignored (Squad's reader ignores them); missing required members are errors.
        CHECK(journal->decode(R"({"seq":1,"type":"t","d":{},"zzz":5})", back).has_value());
        CHECK(!journal->decode(R"({"seq":1,"type":"t"})", back).has_value());
        CHECK(!journal->decode(R"({"type":"t","d":{}})", back).has_value());
        CHECK(!journal->decode("not json", back).has_value());
    }

    // canvas-event-1: the bytes Sub0Canvas's eventJson writes; no seq, no type.
    {
        Record r = fabrictest::canvasRecord("p", "w-1", 3, "item.put@1", R"({"a":1})");
        std::string line;
        canvas->encode(r, line);
        CHECK_EQ(line, std::string{R"({"kind":"item.put@1","writer":{"namespace":"p","writer":"w-1"},"lamport":3,"subject":"k3","time":"2026-11-24T00:00:00Z","payload":{"a":1}})"});
        Record back;
        CHECK(canvas->decode(line, back).has_value());
        CHECK(back == r);
        // Members this version does not know are kept verbatim and written back, byte for byte (A7).
        const std::string withExtra = R"({"kind":"x@2","writer":{"namespace":"","writer":"w"},"lamport":1,"subject":"","time":"","payload":{"k":[1,2]},"future":{"a":true},"n":5})";
        Record kept;
        CHECK(canvas->decode(withExtra, kept).has_value());
        CHECK_EQ(kept.extra, std::string{R"("future":{"a":true},"n":5)"});
        std::string again;
        canvas->encode(kept, again);
        CHECK_EQ(again, withExtra);
        // required members
        CHECK(!canvas->decode(R"({"writer":{"namespace":"","writer":"w"},"lamport":1})", back).has_value());
        CHECK(!canvas->decode(R"({"kind":"k","lamport":1})", back).has_value());
        CHECK(!canvas->decode(R"({"kind":"k","writer":{"namespace":"","writer":"w"}})", back).has_value());
        // defaults: subject, time, payload may be absent
        CHECK(canvas->decode(R"({"kind":"k","writer":{"namespace":"","writer":"w"},"lamport":9})", back).has_value());
        CHECK_EQ(back.payload, std::string{"{}"});
    }

    // Neither shape reads the other's text, and the shape of a line is told by its fields.
    {
        const std::string jl = R"({"seq":1,"ts":1.5,"type":"t","d":{}})";
        const std::string cl = R"({"kind":"k","writer":{"namespace":"","writer":"w"},"lamport":1,"subject":"","time":"","payload":{}})";
        Record scratch;
        CHECK(!canvas->decode(jl, scratch).has_value());
        CHECK(!journal->decode(cl, scratch).has_value());
        auto sj = detectShape(jl);
        auto sc = detectShape(cl);
        CHECK(sj.has_value() && *sj == "journal-1");
        CHECK(sc.has_value() && *sc == "canvas-event-1");
        CHECK(!detectShape(R"({"a":1})").has_value());
        CHECK(!detectShape("[]").has_value());
        // A text with both fields sets is ambiguous, never silently one of them.
        auto both = detectShape(R"({"seq":1,"type":"t","d":{},"kind":"k","writer":{},"lamport":1})");
        CHECK(!both.has_value());
        for (const RecordCodec* c : codecs())
            for (std::string_view f : c->requiredFields()) {
                bool listed = false;
                for (std::string_view g : c->fields()) listed = listed || g == f;
                CHECK(listed);
            }
    }

    // A file Sub0Canvas's own eventJson wrote (tests/fixtures/canvas-event-1): each line is detected, read, and written back to the same bytes.
    {
        std::ifstream in(std::string{SUB0FABRIC_FIXTURE_DIR} + "/canvas-event-1/canvas-written.ndjson", std::ios::binary);
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            auto shape = detectShape(line);
            CHECK(shape.has_value() && *shape == "canvas-event-1");
            Record r;
            CHECK(canvas->decode(line, r).has_value());
            std::string again;
            canvas->encode(r, again);
            CHECK_EQ(again, line);
            if (n == 3) { CHECK_EQ(r.kind, std::string{"future.kind@7"}); CHECK_EQ(r.lamport, 40u); CHECK(!r.extra.empty()); }
        }
        CHECK_EQ(n, 3);
    }

    CHECK(splitKind("item.put@1").name == "item.put");
    CHECK(splitKind("item.put@1").version == "1");
    CHECK(splitKind("note").version.empty());
    return finish();
}
