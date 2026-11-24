// The review's section 5 budgets for F0 as benchmarks (docs/DESIGN.md, "Budgets"): append, `since(cursor)`, merge and parse. Two kinds of result. Timing rows are
// measured locally only (ctest label `timing`, never in CI). The metrics are deterministic facts (counts, bytes, allocations and live heap counted by the replaced
// operator new below, records skipped and bytes read as counted by the reader) judged against perf/metrics.json by the `_metrics` test, which runs in every tier.
// Metrics are registered inside the workloads, once per process: `--metrics-only` and `--smoke` run each workload once. Everything is generated from loops and
// arithmetic, so every machine writes the same bytes.
#define ANKERL_NANOBENCH_IMPLEMENT
#include "bench_runner.hpp"

#include "sub0fabric/io/store.hpp"
#include "sub0fabric/pure/codec.hpp"
#include "sub0fabric/pure/crc32c.hpp"
#include "sub0fabric/pure/segment.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace sub0fabric;
using sub0bench::MetricKind;
namespace fs = std::filesystem;

// ---- a counting allocator: allocation counts and live heap bytes are deterministic facts about the library (single-threaded, plain counters). On MinGW the
// executable links the C++ runtime statically (CMakeLists.txt) so that this operator new is the only one ----------------------------------------------
namespace {
std::size_t gAllocations = 0;   ///< calls to operator new so far
std::size_t gLiveBytes = 0;     ///< bytes requested and not yet freed
std::size_t gPeakBytes = 0;     ///< the most gLiveBytes has been since it was last reset
constexpr std::size_t kHeader = 16;   ///< keeps 16-byte alignment and holds the requested size

void* countedAlloc(std::size_t size)
{
    void* raw = std::malloc(size + kHeader);
    if (raw == nullptr) throw std::bad_alloc();
    std::memcpy(raw, &size, sizeof size);
    ++gAllocations;
    gLiveBytes += size;
    gPeakBytes = std::max(gPeakBytes, gLiveBytes);
    return static_cast<char*>(raw) + kHeader;
}

void countedFree(void* p) noexcept
{
    if (p == nullptr) return;
    char* raw = static_cast<char*>(p) - kHeader;
    std::size_t size = 0;
    std::memcpy(&size, raw, sizeof size);
    gLiveBytes -= size;
    std::free(raw);
}
}  // namespace

void* operator new(std::size_t size) { return countedAlloc(size); }
void* operator new[](std::size_t size) { return countedAlloc(size); }
void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, std::size_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { countedFree(p); }

namespace {

/// Runs `body` once per process however often the workload repeats (a metric is a fact, not a rate; timed iterations do not recompute it).
struct Once
{
    bool done = false;
    void operator()(const std::function<void()>& body)
    {
        if (done) return;
        done = true;
        body();
    }
};

/// A scratch directory under the working directory, removed at exit.
struct Scratch
{
    fs::path root = fs::current_path() / "bench-tmp";
    Scratch() { fs::remove_all(root); fs::create_directories(root); }
    ~Scratch() { std::error_code ec; fs::remove_all(root, ec); }
    [[nodiscard]] fs::path fresh(const std::string& name) const
    {
        const fs::path p = root / name;
        fs::remove_all(p);
        fs::create_directories(p);
        return p;
    }
};
Scratch& scratch() { static Scratch s; return s; }

/// The i-th record of a deterministic canvas stream: the size and shape of an `item.put` of the issues layer.
Record makeRecord(std::uint64_t i)
{
    Record r;
    r.kind = "item.put@1";
    r.lamport = i + 1;
    r.subject = "squad/issues/test/case" + std::to_string(i % 1000);
    r.time = "2026-11-24T10:00:00Z";
    r.payload = R"({"layer":"issues","kind":"test","title":"case )" + std::to_string(i) + R"( fails","state":"open"})";
    return r;
}

/// Write `perWriter` records for each of `writers` writers (lamports interleaved) under `root`, in batches of 1000.
void fill(const fs::path& root, int writers, std::uint64_t perWriter)
{
    for (int w = 0; w < writers; ++w) {
        auto writer = io::StoreWriter::open(root, {"bench", "w-" + std::to_string(w)});
        if (!writer) { std::fprintf(stderr, "bench setup: %s\n", writer.error().c_str()); std::exit(3); }
        std::vector<Record> batch;
        for (std::uint64_t i = 0; i < perWriter; ++i) {
            Record r = makeRecord(i * static_cast<std::uint64_t>(writers) + static_cast<std::uint64_t>(w));
            r.writer = {"bench", "w-" + std::to_string(w)};
            batch.push_back(std::move(r));
            if (batch.size() == 1000 || i + 1 == perWriter) {
                if (!writer->appendBatch(batch)) { std::fprintf(stderr, "bench setup: append failed\n"); std::exit(3); }
                batch.clear();
            }
        }
    }
}

std::uint64_t drainCount(io::StoreReader& reader, const Cursor& cursor)
{
    auto stream = reader.since(cursor);
    if (!stream) { std::fprintf(stderr, "bench: %s\n", stream.error().c_str()); std::exit(3); }
    std::uint64_t n = 0;
    Record r;
    for (;;) {
        auto got = (*stream)->next(r);
        if (!got || !*got) break;
        ++n;
    }
    return n;
}

void appendBenchmarks(sub0bench::BenchRunner& runner)
{
    ankerl::nanobench::Bench& bench = runner.bench();
    const RecordCodec& canvas = *findCodec("canvas-event-1");

    // Encoding one record into a reused buffer (what a writer does per record, no disk): allocations per record must not grow with the log.
    {
        bench.warmup(1000).epochs(11).minEpochIterations(1000);
        Once once;
        std::string buffer;
        std::uint64_t i = 0;
        runner.run("append encode, frames, one record (memory)", [&] {
            once([&] {
                auto perRecord = [&](std::uint64_t n) {
                    std::string reuse;
                    Record r = makeRecord(0);
                    const std::size_t a0 = gAllocations;
                    for (std::uint64_t k = 0; k < n; ++k) {
                        r.seq = k + 1;
                        reuse.clear();
                        (void)appendRecord(reuse, Framing::frames, canvas, r);
                    }
                    return static_cast<double>(gAllocations - a0) / static_cast<double>(n);
                };
                runner.metric("append encode 1e3: allocations per record", perRecord(1000), "n", MetricKind::max);
                runner.metric("append encode 1e5: allocations per record", perRecord(100000), "n", MetricKind::max);
                std::string one;
                (void)appendRecord(one, Framing::frames, canvas, makeRecord(0));
                runner.metric("append frame bytes, one issues-layer item.put", static_cast<double>(one.size()), "B");
                std::string line;
                (void)appendRecord(line, Framing::lines, *findCodec("journal-1"), makeRecord(0));
                runner.metric("append line bytes, the same record as journal-1", static_cast<double>(line.size()), "B");
            });
            Record r = makeRecord(i++);
            r.seq = i;
            buffer.clear();
            (void)appendRecord(buffer, Framing::frames, canvas, r);
            ankerl::nanobench::doNotOptimizeAway(buffer);
        });
    }

    // A batch of 1000 to disk with one flush, and one record flushed per record: the two ends of the durability policy.
    {
        bench.warmup(1).epochs(5).minEpochIterations(1);
        Once once;
        const fs::path dir = scratch().fresh("append-batch");
        auto writer = io::StoreWriter::open(dir, {"bench", "w"});
        std::vector<Record> batch;
        for (int k = 0; k < 1000; ++k) batch.push_back(makeRecord(static_cast<std::uint64_t>(k)));
        runner.run("append batch of 1000 to disk, one flush (frames)", [&] {
            once([&] {
                const fs::path d2 = scratch().fresh("append-metrics");
                auto w = io::StoreWriter::open(d2, {"bench", "w"});
                std::vector<Record> b;
                const std::size_t a0 = gAllocations;
                std::uint64_t total = 0;
                for (int round = 0; round < 100; ++round) {   // 1e5 records
                    b.clear();
                    for (int k = 0; k < 1000; ++k) b.push_back(makeRecord(total + static_cast<std::uint64_t>(k)));
                    total += 1000;
                    (void)w->appendBatch(b);
                }
                const std::size_t allocs = gAllocations - a0;
                runner.metric("append batch 1e5 to disk: records", static_cast<double>(w->nextSeq() - 1), "n");
                runner.metric("append batch 1e5 to disk: segments", static_cast<double>(w->segmentNumber()), "n");
                runner.metric("append batch 1e5 to disk: allocations per record, record construction included", static_cast<double>(allocs) / 1e5, "n", MetricKind::max);
            });
            (void)writer->appendBatch(batch);
        });
        bench.warmup(1).epochs(5).minEpochIterations(1);
        const fs::path dir2 = scratch().fresh("append-each");
        io::WriterOptions each;
        each.durability.mode = io::Durability::Mode::perRecord;
        auto w2 = io::StoreWriter::open(dir2, {"bench", "w"}, each);
        Record one = makeRecord(7);
        runner.run("append one record to disk, flushed per record", [&] { (void)w2->append(one); });
    }
}

void sinceBenchmarks(sub0bench::BenchRunner& runner)
{
    ankerl::nanobench::Bench& bench = runner.bench();
    const fs::path big = scratch().fresh("since-1e5");
    const fs::path small = scratch().fresh("since-1e4");
    fill(big, 1, 100000);
    fill(small, 1, 10000);

    // Opening scans each segment once into its sparse index: the cost of a cold start, in bytes and records.
    {
        bench.warmup(1).epochs(5).minEpochIterations(1);
        Once once;
        runner.run("reader open and scan, 1e5 records (frames, checksums only)", [&] {
            once([&] {
                const std::size_t a0 = gAllocations;
                auto r = io::StoreReader::open(big);
                runner.metric("reader open 1e5: records scanned", static_cast<double>(r->stats().recordsScanned), "n");
                runner.metric("reader open 1e5: allocations, sparse index and segment table", static_cast<double>(gAllocations - a0), "n", MetricKind::max);
            });
            auto r = io::StoreReader::open(big);
            ankerl::nanobench::doNotOptimizeAway(r);
        });
    }

    // since(cursor): the cost is the records returned, not the log. The same tail read at 1e4 and at 1e5 reads the same bytes and skips under one stride.
    {
        bench.warmup(10).epochs(11).minEpochIterations(1);
        Once once;
        auto readerBig = io::StoreReader::open(big);
        auto readerSmall = io::StoreReader::open(small);
        Cursor atBig, atSmall;
        atBig.set({"bench", "w-0"}, 99900);
        atSmall.set({"bench", "w-0"}, 9900);
        runner.run("since(cursor): tail of 100 from a log of 1e5 (warm reader)", [&] {
            once([&] {
                auto measure = [&](io::StoreReader& rd, const Cursor& c, const char* tag) {
                    const io::ReadStats before = rd.stats();
                    const std::size_t a0 = gAllocations;
                    const std::uint64_t n = drainCount(rd, c);
                    const io::ReadStats after = rd.stats();
                    const std::string t{tag};
                    runner.metric(("since tail 100 of " + t + ": records returned").c_str(), static_cast<double>(n), "n");
                    runner.metric(("since tail 100 of " + t + ": records skipped after the index seek").c_str(), static_cast<double>(after.recordsSkipped - before.recordsSkipped), "n", MetricKind::max);
                    runner.metric(("since tail 100 of " + t + ": bytes read").c_str(), static_cast<double>(after.bytesRead - before.bytesRead), "B", MetricKind::max);
                    runner.metric(("since tail 100 of " + t + ": records rescanned").c_str(), static_cast<double>(after.recordsScanned - before.recordsScanned), "n");
                    runner.metric(("since tail 100 of " + t + ": allocations").c_str(), static_cast<double>(gAllocations - a0), "n", MetricKind::max);
                };
                measure(*readerSmall, atSmall, "1e4");
                measure(*readerBig, atBig, "1e5");
                // And from the very start of 1e5: everything is returned, and nothing is skipped.
                const io::ReadStats before = readerBig->stats();
                const std::uint64_t all = drainCount(*readerBig, Cursor{});
                runner.metric("since from the start of 1e5: records returned", static_cast<double>(all), "n");
                runner.metric("since from the start of 1e5: records skipped", static_cast<double>(readerBig->stats().recordsSkipped - before.recordsSkipped), "n");
            });
            ankerl::nanobench::doNotOptimizeAway(drainCount(*readerBig, atBig));
        });
        runner.run("since(cursor): tail of 100 from a log of 1e4 (warm reader)", [&] { ankerl::nanobench::doNotOptimizeAway(drainCount(*readerSmall, atSmall)); });
    }
}

void mergeBenchmarks(sub0bench::BenchRunner& runner)
{
    ankerl::nanobench::Bench& bench = runner.bench();
    const fs::path big = scratch().fresh("merge-1e5");
    const fs::path small = scratch().fresh("merge-1e4");
    fill(big, 16, 6250);
    fill(small, 16, 625);

    // The streaming merge over 16 writers: live heap while draining is a function of the number of streams, not of records.
    {
        bench.warmup(1).epochs(5).minEpochIterations(1);
        Once once;
        auto readerBig = io::StoreReader::open(big);
        auto readerSmall = io::StoreReader::open(small);
        runner.run("merge 16 writers x 6250 records from disk (streaming, lamport)", [&] {
            once([&] {
                auto peakOf = [&](io::StoreReader& rd, std::uint64_t& count, std::uint64_t& outOfOrder) {
                    const std::size_t base = gLiveBytes;
                    gPeakBytes = gLiveBytes;
                    auto stream = rd.since(Cursor{});
                    Record r, previous;
                    count = 0;
                    outOfOrder = 0;
                    for (;;) {
                        auto got = (*stream)->next(r);
                        if (!got || !*got) break;
                        if (count > 0 && (r.lamport < previous.lamport)) ++outOfOrder;
                        previous = r;
                        ++count;
                    }
                    return gPeakBytes - base;
                };
                std::uint64_t nSmall = 0, nBig = 0, oSmall = 0, oBig = 0;
                const std::size_t peakSmall = peakOf(*readerSmall, nSmall, oSmall);
                const std::size_t peakBig = peakOf(*readerBig, nBig, oBig);
                runner.metric("merge 16 streams 1e4: records out", static_cast<double>(nSmall), "n");
                runner.metric("merge 16 streams 1e5: records out", static_cast<double>(nBig), "n");
                runner.metric("merge 16 streams 1e5: records out of order", static_cast<double>(oBig + oSmall), "n");
                runner.metric("merge 16 streams 1e4: peak live heap bytes", static_cast<double>(peakSmall), "B", MetricKind::max);
                runner.metric("merge 16 streams 1e5: peak live heap bytes", static_cast<double>(peakBig), "B", MetricKind::max);
                runner.metric("merge 16 streams: peak at 1e5 per peak at 1e4, in thousandths", 1000.0 * static_cast<double>(peakBig) / static_cast<double>(peakSmall), "n", MetricKind::max);
            });
            ankerl::nanobench::doNotOptimizeAway(drainCount(*readerBig, Cursor{}));
        });
    }
}

void parseBenchmarks(sub0bench::BenchRunner& runner)
{
    ankerl::nanobench::Bench& bench = runner.bench();
    const RecordCodec& canvas = *findCodec("canvas-event-1");
    const RecordCodec& journal = *findCodec("journal-1");
    std::string canvasLine, journalLine;
    {
        Record r = makeRecord(12345);
        r.writer = {"bench", "w-0"};
        canvas.encode(r, canvasLine);
        r.seq = 12346;
        r.ts = 1766000000.5;
        journal.encode(r, journalLine);
    }

    bench.warmup(1000).epochs(11).minEpochIterations(1000);
    Once once;
    Record scratchRecord;
    runner.run("decode canvas-event-1 line (simdjson on-demand)", [&] {
        once([&] {
            (void)canvas.decode(canvasLine, scratchRecord);   // warm: the parser pool and the record's strings
            (void)journal.decode(journalLine, scratchRecord);
            const std::size_t a0 = gAllocations;
            for (int k = 0; k < 1000; ++k) (void)canvas.decode(canvasLine, scratchRecord);
            runner.metric("decode canvas-event-1: allocations per record, record reused", static_cast<double>(gAllocations - a0) / 1000.0, "n", MetricKind::max);
            const std::size_t b0 = gAllocations;
            for (int k = 0; k < 1000; ++k) (void)journal.decode(journalLine, scratchRecord);
            runner.metric("decode journal-1: allocations per record, record reused", static_cast<double>(gAllocations - b0) / 1000.0, "n", MetricKind::max);
            runner.metric("decode canvas-event-1: line bytes", static_cast<double>(canvasLine.size()), "B");
            std::string pattern(1000, '\0');
            for (std::size_t k = 0; k < pattern.size(); ++k) pattern[k] = static_cast<char>(k * 31 + 7);
            runner.metric("crc32c of a 1000-byte pattern", static_cast<double>(crc32c(pattern)), "n");
        });
        (void)canvas.decode(canvasLine, scratchRecord);
        ankerl::nanobench::doNotOptimizeAway(scratchRecord);
    });
    runner.run("decode journal-1 line (simdjson on-demand)", [&] {
        (void)journal.decode(journalLine, scratchRecord);
        ankerl::nanobench::doNotOptimizeAway(scratchRecord);
    });
    std::string block(4096, 'x');
    runner.run("crc32c of 4 KiB (slice-by-8)", [&] { ankerl::nanobench::doNotOptimizeAway(crc32c(block)); });
}

}  // namespace

int main(int argc, char** argv)
{
    sub0bench::BenchRunner runner(argc, argv, "sub0fabric");
    appendBenchmarks(runner);
    sinceBenchmarks(runner);
    mergeBenchmarks(runner);
    parseBenchmarks(runner);
    return runner.finish();
}
