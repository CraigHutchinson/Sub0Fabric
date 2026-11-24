// The query protocol `fabric-query-1` (pure/protocol.hpp) over the in-memory query source (pure/query_source.hpp), one block per contract property of
// docs/DESIGN.md "Protocol (W3a)". The same cases run over the directory store in test_protocol_store.
#include "helpers.hpp"
#include "sub0fabric/pure/query_source.hpp"
#include "sub0platform/testing/check.hpp"

using namespace sub0fabric;
using namespace fabrictest;

namespace {

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

/// `n` canvas records for one writer with the given lamports.
std::vector<Record> canvasStream(const std::string& ns, const std::string& writer, std::vector<std::uint64_t> lamports)
{
    std::vector<Record> v;
    for (std::uint64_t l : lamports) v.push_back(canvasRecord(ns, writer, l));
    return v;
}

}  // namespace

int main()
{
    // The query source seam, memory implementation: add sets seq, checks the shape, defaults the order, lists by writer.
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

    // Order resolution: a stream's own order, an override, mixed streams without an override are the store's error text, an unknown name.
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
        // No streams: lamport, an empty read.
        MemoryQuerySource empty;
        auto e = empty.since(Cursor{}, {});
        CHECK(e.has_value() && drain(**e).empty());
        // An explicit order on a stream, as a segment header would say it.
        MemoryQuerySource bySeq;
        CHECK(bySeq.add({"p", "a"}, "canvas-event-1", canvasStream("p", "a", {5, 6}), "seq").has_value());
        CHECK_EQ(bySeq.streams()[0].order, std::string{"seq"});
    }
    return finish();
}
