// The directory store (io/store.hpp): per-writer segments, the merging reader, cursors, recovery after a crash at every byte, rollover, the sparse index
// as a measured fact, both framings and both shapes, and the writer's refusals.
#include "helpers.hpp"
#include "sub0fabric/io/cursor_file.hpp"
#include "sub0fabric/io/store.hpp"
#include "sub0platform/testing/check.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace sub0fabric;
using namespace sub0fabric::io;
using namespace fabrictest;
namespace fs = std::filesystem;

namespace {

std::vector<Record> drain(StoreReader& reader, const Cursor& cursor, std::string_view order = {})
{
    std::vector<Record> out;
    auto stream = reader.since(cursor, order);
    CHECK(stream.has_value());
    if (!stream) return out;
    for (Record r;;) {
        auto got = (*stream)->next(r);
        CHECK(got.has_value());
        if (!got || !*got) break;
        out.push_back(r);
    }
    return out;
}

WriterOptions withShape(std::string shape) { WriterOptions o; o.shape = std::move(shape); return o; }
WriterOptions withOrder(std::string order) { WriterOptions o; o.order = std::move(order); return o; }

std::string slurp(const fs::path& p) { std::ifstream in(p, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
void spit(const fs::path& p, std::string_view s) { fs::create_directories(p.parent_path()); std::ofstream o(p, std::ios::binary); o.write(s.data(), static_cast<std::streamsize>(s.size())); }

/// Append `n` records with lamport values `start`, `start + step`, ... to a writer.
void put(StoreWriter& w, int n, std::uint64_t start, std::uint64_t step)
{
    for (int i = 0; i < n; ++i) {
        Record r = canvasRecord("", "", start + static_cast<std::uint64_t>(i) * step, "item.put@1", R"({"i":)" + std::to_string(i) + "}");
        CHECK(w.append(r).has_value());
    }
}

}  // namespace

int main()
{
    const fs::path root = makeTempDir("store");

    // Three writers (two namespaces), one segment each, interleaved lamports: the merge is (lamport, writer) over all of them.
    {
        auto a = StoreWriter::open(root, {"proj", "w-1"});
        auto b = StoreWriter::open(root, {"proj", "w-2"});
        auto c = StoreWriter::open(root, {"other", "w-1"});
        CHECK(a.has_value() && b.has_value() && c.has_value());
        if (!a || !b || !c) return finish();
        put(*a, 50, 1, 3);   // 1, 4, 7, ...
        put(*b, 50, 2, 3);   // 2, 5, 8, ...
        put(*c, 20, 3, 6);   // 3, 9, 15, ... (ties with a and b at 4? no: 3 mod 6 == 3)
        CHECK_EQ(a->nextSeq(), 51u);
        CHECK(a->sync().has_value());
        CHECK(fs::exists(root / "proj" / "w-1" / "seg-000001.s0f"));
        CHECK(fs::exists(root / "other" / "w-1" / "seg-000001.s0f"));
    }
    auto reader = StoreReader::open(root);
    CHECK(reader.has_value());
    if (!reader) return finish();
    CHECK_EQ(reader->streams().size(), 3u);
    CHECK_EQ(reader->count({"proj", "w-1"}), 50u);
    CHECK_EQ(reader->count({"nobody", "x"}), 0u);
    const auto all = drain(*reader, Cursor{});
    CHECK_EQ(all.size(), 120u);
    for (std::size_t i = 1; i < all.size(); ++i) CHECK(all[i - 1].lamport < all[i].lamport || (all[i - 1].lamport == all[i].lamport && all[i - 1].writer < all[i].writer));
    CHECK(all.front().writer == (WriterId{"proj", "w-1"}) && all.front().lamport == 1);
    for (const Record& r : all) CHECK(r.seq >= 1 && r.seq <= 50);

    // Cursors: read half, persist, append, refresh, and since(cursor) gives exactly the rest once.
    {
        Cursor cursor;
        const std::size_t half = 60;
        for (std::size_t i = 0; i < half; ++i) cursor.advance(all[i]);
        const fs::path cursorFile = root / "reader.cursor";
        CHECK(saveCursor(cursorFile, cursor).has_value());
        auto loaded = loadCursor(cursorFile);
        CHECK(loaded.has_value() && *loaded == cursor);
        CHECK(loadCursor(root / "no-such.cursor").value() == Cursor{});
        const auto rest = drain(*reader, *loaded);
        CHECK_EQ(rest.size(), all.size() - half);
        // Every record is delivered exactly once across the two reads: (writer, seq) is unique and complete.
        std::vector<std::pair<WriterId, std::uint64_t>> seen;
        for (std::size_t i = 0; i < half; ++i) seen.emplace_back(all[i].writer, all[i].seq);
        for (const Record& r : rest) seen.emplace_back(r.writer, r.seq);
        std::ranges::sort(seen);
        CHECK(std::ranges::adjacent_find(seen) == seen.end());
        CHECK_EQ(seen.size(), 120u);

        // A writer appends while the reader is open: refresh extends, and the cursor sees only the new records.
        auto a = StoreWriter::open(root, {"proj", "w-1"});   // reopening continues after the last record
        CHECK(a.has_value() && a->nextSeq() == 51u);
        put(*a, 5, 1000, 1);
        CHECK(a->sync().has_value());
        Cursor atEnd;
        for (const Record& r : all) atEnd.advance(r);
        CHECK_EQ(drain(*reader, atEnd).size(), 0u);   // not yet refreshed
        CHECK(reader->refresh().has_value());
        const auto fresh = drain(*reader, atEnd);
        CHECK_EQ(fresh.size(), 5u);
        for (const Record& r : fresh) CHECK(r.writer == (WriterId{"proj", "w-1"}) && r.seq > 50);
    }

    // Two writers with one id are refused (the lock); another id is fine; a record naming another writer is refused; a stream keeps its shape.
    {
        auto first = StoreWriter::open(root, {"proj", "w-9"});
        CHECK(first.has_value());
        auto second = StoreWriter::open(root, {"proj", "w-9"});
        CHECK(!second.has_value());
        auto other = StoreWriter::open(root, {"proj", "w-10"});
        CHECK(other.has_value());
        Record stranger = canvasRecord("proj", "w-2", 1);
        CHECK(!first->append(stranger).has_value());
        first = std::unexpected(std::string{});   // release the lock
        WriterOptions journalShape;
        journalShape.shape = "journal-1";
        CHECK(!StoreWriter::open(root, {"proj", "w-9"}, journalShape).has_value());   // the stream is canvas-event-1
        CHECK(!StoreWriter::open(root, {"proj", "w-11"}, withShape("no-such-shape")).has_value());
        CHECK(!StoreWriter::open(root, {"proj", "w-12"}, withOrder("no-such-order")).has_value());
    }

    // Crash at every byte: cut a writer's only segment anywhere; the reader sees a clean prefix, the writer recovers, appends continue, nothing earlier is lost.
    {
        const fs::path base = makeTempDir("crash");
        {
            auto w = StoreWriter::open(base, {"p", "w"});
            CHECK(w.has_value());
            put(*w, 5, 1, 1);
        }
        const std::string seg = slurp(base / "p" / "w" / "seg-000001.s0f");
        std::size_t recoveredSomething = 0;
        for (std::size_t cut = 0; cut <= seg.size(); ++cut) {
            const fs::path d = makeTempDir("cut");
            spit(d / "p" / "w" / "seg-000001.s0f", std::string_view{seg}.substr(0, cut));
            auto rd = StoreReader::open(d);
            CHECK(rd.has_value());
            if (!rd) { removeTree(d); continue; }
            const auto before = drain(*rd, Cursor{});
            CHECK(before.size() <= 5u);
            for (std::size_t i = 0; i < before.size(); ++i) CHECK_EQ(before[i].seq, i + 1);
            {
                auto w = StoreWriter::open(d, {"p", "w"});
                CHECK(w.has_value());
                if (w) {
                    CHECK_EQ(w->nextSeq(), before.size() + 1);
                    if (w->recoveredBytes() > 0) ++recoveredSomething;
                    Record r = canvasRecord("", "", 100);
                    CHECK(w->append(r).has_value());
                }
            }
            auto rd2 = StoreReader::open(d);
            CHECK(rd2.has_value());
            if (rd2) {
                const auto after = drain(*rd2, Cursor{});
                CHECK_EQ(after.size(), before.size() + 1);
                CHECK(rd2->streams()[0].damage.empty() && !rd2->streams()[0].tornTail);
            }
            removeTree(d);
        }
        CHECK(recoveredSomething > 100);
        removeTree(base);
    }

    // Damage before the end: the writer will not bury it, the reader reports it, and the read stops there with the records before it delivered first.
    {
        const fs::path base = makeTempDir("damage");
        {
            auto w = StoreWriter::open(base, {"p", "w"});
            put(*w, 6, 1, 1);
        }
        const fs::path file = base / "p" / "w" / "seg-000001.s0f";
        std::string seg = slurp(file);
        seg[seg.size() * 2 / 5] = static_cast<char>(seg[seg.size() * 2 / 5] ^ 0x20);
        spit(file, seg);
        CHECK(!StoreWriter::open(base, {"p", "w"}).has_value());
        auto rd = StoreReader::open(base);
        CHECK(rd.has_value());
        if (rd) {
            CHECK(!rd->streams()[0].damage.empty());
            auto stream = rd->since(Cursor{});
            CHECK(stream.has_value());
            if (stream) {
                std::size_t good = 0;
                bool sawError = false;
                for (Record r;;) {
                    auto got = (*stream)->next(r);
                    if (!got) { sawError = true; break; }
                    if (!*got) break;
                    ++good;
                }
                CHECK(sawError);
                CHECK_EQ(good, rd->count({"p", "w"}));
            }
        }
        removeTree(base);
    }

    // Rollover: a small limit makes many segments; positions continue; since(cursor) from anywhere equals the model; headers carry `first`.
    {
        const fs::path base = makeTempDir("roll");
        WriterOptions o;
        o.maxSegmentBytes = 1500;
        std::vector<Record> model;
        {
            auto w = StoreWriter::open(base, {"p", "w"}, o);
            CHECK(w.has_value());
            for (int i = 0; i < 200; ++i) {
                Record r = canvasRecord("", "", static_cast<std::uint64_t>(i) + 1, "item.put@1", R"({"pad":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"})");
                CHECK(w->append(r).has_value());
                model.push_back(r);
            }
            CHECK(w->segmentNumber() > 5);
        }
        const auto files = fs::directory_iterator(base / "p" / "w");
        std::size_t segs = 0;
        for (const auto& e : files) segs += e.path().extension() == ".s0f";
        CHECK(segs > 5);
        ReaderOptions ro;
        ro.indexEvery = 8;
        auto rd = StoreReader::open(base, ro);
        CHECK(rd.has_value());
        if (rd) {
            CHECK_EQ(rd->streams()[0].records, 200u);
            CHECK_EQ(rd->streams()[0].segments, segs);
            for (std::uint64_t from : {0u, 1u, 7u, 8u, 9u, 63u, 64u, 100u, 150u, 199u, 200u, 250u}) {
                Cursor c;
                c.set({"p", "w"}, from);
                const auto got = drain(*rd, c);
                const std::size_t expected = from >= 200 ? 0 : 200 - from;
                CHECK_EQ(got.size(), expected);
                for (std::size_t i = 0; i < got.size(); ++i) CHECK_EQ(got[i].seq, from + i + 1);
            }
            // A writer reopening a rolled directory continues in the last segment, at the right position.
            auto w = StoreWriter::open(base, {"p", "w"}, o);
            CHECK(w.has_value() && w->nextSeq() == 201u);
        }
        removeTree(base);
    }

    // The sparse index, as a fact: a read from the middle of 20,000 records skips fewer than `indexEvery` and reads far less than the file.
    {
        const fs::path base = makeTempDir("index");
        {
            auto w = StoreWriter::open(base, {"p", "w"});
            std::vector<Record> batch;
            for (int i = 0; i < 20000; ++i) batch.push_back(canvasRecord("", "", static_cast<std::uint64_t>(i) + 1));
            CHECK(w->appendBatch(batch).value() == 20000u);
        }
        auto rd = StoreReader::open(base);
        CHECK(rd.has_value());
        if (rd) {
            const std::uint64_t scanned = rd->stats().recordsScanned;
            CHECK_EQ(scanned, 20000u);
            const std::uint64_t bytesAfterScan = rd->stats().bytesRead;
            Cursor c;
            c.set({"p", "w"}, 19900);
            const auto tail = drain(*rd, c);
            CHECK_EQ(tail.size(), 100u);
            CHECK(rd->stats().recordsSkipped < 64);
            CHECK_EQ(rd->stats().recordsReturned, 100u);
            const std::uint64_t bytesForTail = rd->stats().bytesRead - bytesAfterScan;
            CHECK(bytesForTail <= (64u << 10));   // one chunk, not the 2.7 MB file
            // Reading again does not rescan: the scan counters do not move.
            CHECK_EQ(rd->stats().recordsScanned, 20000u);
        }
        removeTree(base);
    }

    // Lines framing in the per-writer layout: the shape is found from the file; two shapes with two order policies need a named order.
    {
        const fs::path base = makeTempDir("lines");
        WriterOptions jo;
        jo.shape = "journal-1";
        jo.framing = Framing::lines;
        {
            auto w = StoreWriter::open(base, {"squad", "j"}, jo);
            CHECK(w.has_value());
            for (int i = 0; i < 5; ++i) { Record r = journalRecord("t", R"({"n":)" + std::to_string(i) + "}"); CHECK(w->append(r).has_value()); }
            auto c = StoreWriter::open(base, {"canvas", "c"});
            put(*c, 5, 1, 1);
        }
        CHECK(fs::exists(base / "squad" / "j" / "journal-000001.ndjson"));
        auto rd = StoreReader::open(base);
        CHECK(rd.has_value());
        if (rd) {
            const auto streams = rd->streams();
            CHECK_EQ(streams.size(), 2u);
            auto mixed = rd->since(Cursor{});
            CHECK(!mixed.has_value());   // 'seq' and 'lamport' streams: name one
            const auto asSeq = drain(*rd, Cursor{}, "seq");
            CHECK_EQ(asSeq.size(), 10u);
            for (std::size_t i = 1; i < asSeq.size(); ++i) CHECK(asSeq[i - 1].seq <= asSeq[i].seq);
            CHECK(!rd->since(Cursor{}, "no-such-order").has_value());
            // journal-1 records get the stream's writer stamped on read (the line does not carry one).
            for (const Record& r : asSeq) if (r.kind == "t") CHECK(r.writer == (WriterId{"squad", "j"}));
        }
        removeTree(base);
    }

    // The shared layout: one stream in the root, a lock, whatever the writer id.
    {
        const fs::path base = makeTempDir("shared");
        WriterOptions so;
        so.layout = Layout::shared;
        {
            auto w = StoreWriter::open(base, {"p", "w"}, so);
            CHECK(w.has_value());
            put(*w, 3, 1, 1);
            CHECK(!StoreWriter::open(base, {"p", "w"}, so).has_value());   // locked
        }
        CHECK(fs::exists(base / "seg-000001.s0f"));
        auto rd = StoreReader::open(base);
        CHECK(rd.has_value() && drain(*rd, Cursor{}).size() == 3u);
        removeTree(base);
    }

    // Durability policies and batches.
    {
        const fs::path base = makeTempDir("dur");
        for (auto mode : {Durability::Mode::perRecord, Durability::Mode::perBatch, Durability::Mode::interval}) {
            WriterOptions o;
            o.durability.mode = mode;
            o.durability.interval = std::chrono::milliseconds{0};
            auto w = StoreWriter::open(base, {"d", std::to_string(static_cast<int>(mode))}, o);
            CHECK(w.has_value());
            put(*w, 3, 1, 1);
            std::vector<Record> batch{canvasRecord("", "", 10), canvasRecord("", "", 11)};
            auto last = w->appendBatch(batch);
            CHECK(last.has_value() && *last == 5u && batch[0].seq == 4u && batch[1].seq == 5u);
            CHECK(w->appendBatch({}).value() == 5u);   // an empty batch changes nothing
        }
        auto rd = StoreReader::open(base);
        CHECK(rd.has_value() && drain(*rd, Cursor{}).size() == 15u);
        removeTree(base);
    }

    // A missing root is an empty store.
    {
        auto rd = StoreReader::open(root / "does" / "not" / "exist");
        CHECK(rd.has_value() && rd->streams().empty() && drain(*rd, Cursor{}).empty());
    }

    removeTree(root);
    return finish();
}
