// journal-1 on Fabric: the behaviour of Sub0Squad's `Journal`, preserved (review D1). The first block is Sub0Squad's tests/test_journal.cpp, ported with only the
// include and the namespace changed; the rest holds what that test does not: a journal Squad's own code wrote, read and written back byte for byte, the replay
// error text, an existing journal continued, and the same directory read through the merging reader.
#include "sub0fabric/io/journal1.hpp"
#include "sub0fabric/io/store.hpp"
#include "sub0fabric/pure/codec.hpp"
#include "sub0platform/testing/check.hpp"

#include <fstream>
#include <sstream>

using namespace sub0fabric::io;
namespace fs = std::filesystem;

static std::string slurp(const fs::path& p) { std::ifstream in(p, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }

int main()
{
    // ---- Sub0Squad's tests/test_journal.cpp, as it is -------------------------------------------------------------------------------------------------
    const fs::path dir = makeTempDir("journal");
    {
        auto j = Journal::open(dir);
        CHECK(j.has_value());
        CHECK(j->append("a", "{\"x\":1}").has_value());
        CHECK(j->append("b", "{\"s\":\"q\\\"uote\\n\"}").has_value());
        CHECK(j->append("c", "{}", false).has_value());
    }
    auto r = Journal::replay(dir);
    CHECK(r.has_value());
    CHECK_EQ(r->records.size(), 3u);
    CHECK_EQ(r->records[1].type, std::string{"b"});
    CHECK(!r->tornTail);

    // Truncation fuzz: cut the file at every byte; replay must never fail and must return a prefix of the records.
    const std::string full = slurp(dir / "journal-000001.ndjson");
    for (std::size_t cut = 0; cut <= full.size(); ++cut) {
        const fs::path d2 = makeTempDir("jcut");
        { std::ofstream o(d2 / "journal-000001.ndjson", std::ios::binary); o.write(full.data(), static_cast<std::streamsize>(cut)); }
        auto rr = Journal::replay(d2);
        CHECK(rr.has_value());
        if (rr) {
            CHECK(rr->records.size() <= 3u);
            for (std::size_t i = 0; i < rr->records.size(); ++i) CHECK_EQ(rr->records[i].seq, i + 1);
            // Reopening truncates the torn tail; a new append continues the sequence and replays cleanly.
            auto j2 = Journal::open(d2);
            CHECK(j2.has_value());
            if (j2) {
                CHECK(j2->append("n", "{}").has_value());
                auto r3 = Journal::replay(d2);
                CHECK(r3.has_value() && !r3->tornTail && r3->records.size() == rr->records.size() + 1);
            }
        }
        removeTree(d2);
    }

    // Corruption in the middle is reported, not silently skipped.
    {
        const fs::path d3 = makeTempDir("jbad");
        std::string bad = full;
        bad[bad.find('\n') / 2] = '#';
        { std::ofstream o(d3 / "journal-000001.ndjson", std::ios::binary); o << bad; }
        auto rep = Journal::replay(d3);
        CHECK(!rep.has_value());
        // The message is Squad's: where the bad line starts, and in which file.
        if (!rep) CHECK(rep.error().starts_with("journal corrupt before its end: line starting at byte 0 in "));
        removeTree(d3);
    }
    removeTree(dir);

    // ---- beyond Squad's test ---------------------------------------------------------------------------------------------------------------------------
    // No journal yet: an empty replay, not an error.
    {
        const fs::path none = makeTempDir("jnone");
        auto rep = Journal::replay(none);
        CHECK(rep.has_value() && rep->records.empty() && !rep->tornTail && rep->validBytes == 0);
        removeTree(none);
    }

    // A journal Sub0Squad's own code wrote (tests/fixtures/journal-1): Fabric reads it and writes every record back to the same bytes.
    {
        const std::string fixture = std::string{SUB0FABRIC_FIXTURE_DIR} + "/journal-1/squad-written.ndjson";
        const std::string text = slurp(fixture);
        CHECK(!text.empty());
        const fs::path d = makeTempDir("jfix");
        { std::ofstream o(d / "journal-000001.ndjson", std::ios::binary); o << text; }
        auto rep = Journal::replay(d);
        CHECK(rep.has_value());
        if (rep) {
            CHECK_EQ(rep->records.size(), 4u);
            CHECK_EQ(rep->validBytes, text.size());
            const sub0fabric::RecordCodec& codec = *sub0fabric::findCodec("journal-1");
            std::string rewritten;
            for (const auto& jr : rep->records) {
                sub0fabric::Record rec;
                rec.seq = jr.seq;
                rec.ts = jr.ts;
                rec.kind = jr.type;
                rec.payload = jr.body;
                codec.encode(rec, rewritten);
                rewritten.push_back('\n');
            }
            CHECK_EQ(rewritten, text);
            CHECK_EQ(rep->records[3].body, std::string{R"({"id":"j-1","to":"done","n":12345678901234,"f":1.5e-3})"});
        }
        // Continue it: the next record is 5, and Fabric's line follows Squad's without a seam.
        auto j = Journal::open(d);
        CHECK(j.has_value());
        if (j) {
            CHECK_EQ(j->nextSeq(), 5u);
            CHECK_EQ(j->records().size(), 4u);
            auto seq = j->append("job.state", R"({"id":"j-1","to":"archived"})");
            CHECK(seq.has_value() && *seq == 5u);
        }
        auto again = Journal::replay(d);
        CHECK(again.has_value() && again->records.size() == 5u && !again->tornTail);
        const std::string after = slurp(d / "journal-000001.ndjson");
        CHECK(after.starts_with(text));
        CHECK(after.ends_with("\n"));

        // The same directory read through the merging reader: a lines stream of shape journal-1, found by its fields, in seq order.
        auto reader = StoreReader::open(d);
        CHECK(reader.has_value());
        if (reader) {
            const auto streams = reader->streams();
            CHECK_EQ(streams.size(), 1u);
            if (!streams.empty()) {
                CHECK_EQ(streams[0].shape, std::string{"journal-1"});
                CHECK_EQ(streams[0].order, std::string{"seq"});
                CHECK_EQ(streams[0].records, 5u);
                CHECK(streams[0].framing == sub0fabric::Framing::lines);
            }
            auto merged = reader->since(sub0fabric::Cursor{});
            CHECK(merged.has_value());
            std::uint64_t expectSeq = 1;
            sub0fabric::Record rec;
            while (merged) {
                auto got = (*merged)->next(rec);
                CHECK(got.has_value());
                if (!got || !*got) break;
                CHECK_EQ(rec.seq, expectSeq);
                ++expectSeq;
            }
            CHECK_EQ(expectSeq, 6u);
        }
        removeTree(d);
    }
    return finish();
}
