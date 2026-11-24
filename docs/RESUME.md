# Where Sub0Fabric stands

Status: living. Rewritten at the end of each working session. Last: 2026-11-24 (F0 built; nothing pushed, nothing tagged).

Read [DESIGN.md](DESIGN.md) for the shape and the byte formats, [BACKLOG.md](BACKLOG.md) for what is next. The decisions are the architecture review's (Sub0Canvas
`docs/reviews/2026-11-23-fabric-portal-store.md`, D1-D11 and section 5); this repository is its wave W2.

## Built (F0, the log core)

| part | state | evidence |
|---|---|---|
| scaffold: layers `pure` and `io`, CPM, Version.cmake, test tiers, presets, CI (smoke, full), `.ci/local.json`, pre-push gate, `SUB0FABRIC_PERF_GATE` | built | `layering`, `test_reporting`, `hook_check.py` (5 pass), `local_ci.py --level quick` |
| record shapes `journal-1` and `canvas-event-1` with disjoint field sets, detection by fields | built | `test_codec`; fixtures written by Squad's own `Journal` and Canvas's own `eventJson` read and written back byte for byte |
| segment formats: `lines` (the journal-1 file) and `frames` (header, length-checked, CRC-32C), torn-tail and corruption rules, zero tails, incremental scanner | built | `test_segment`: truncation at every byte for both framings, chunked feed at every piece size |
| order policies as data (`seq`, `lamport`), streaming k-way merge, per-writer cursors, sparse index | built | `test_order` |
| directory store: writer (recovery, rollover, durability policy, lock), merging reader (`since(cursor)`, `refresh`), cursor files, per-writer and shared layouts | built | `test_store`: crash at every byte, damage before the tail, rollover, index seeks as counted facts |
| journal-1 facade with Squad's `Journal` API; Squad's journal test ported | built | `test_journal1` |
| section 5 budgets as `Sub0Fabric_Bench`: 31 deterministic metrics (every tier), 10 timing rows, first baseline | built | `perf/metrics.json`, `perf/baselines/croglegion.json` |

`ctest --preset quick` and `--preset full`: 10 of 10 pass (GCC 16, MinGW-w64 UCRT, Ninja). The tests take about 14 s together; `test_store` and `test_journal1` are the slow ones
(truncation at every byte, each with its own directories). Clang 22 builds Fabric's own code clean (checked with Platform's `strerror` deprecation silenced) but fails inside
Sub0Platform's `file.cpp` on this machine, as Sub0Canvas's optional clang step does.

## Measured (first baseline, `croglegion`, mains power, Balanced, 5 rounds, CV under 4 %)

| row | median |
|---|---|
| append, batch of 1000 to disk, one flush | 2.0 ms (2.0 us per record) |
| append, one record flushed per record | 2.1 ms (the device flush dominates) |
| append, encode one record (memory) | 0.3 us |
| decode one canvas-event-1 line / one journal-1 line | 0.1 us each |
| reader open and scan of 1e5 records (checksums only) | 13 ms |
| since(cursor): tail of 100 from 1e4 / from 1e5 records, warm reader | 95 us / 106 us (flat in the log's length) |
| merge of 16 writers x 6250 records from disk, streaming | 46 ms (0.46 us per record) |

Deterministic: a tail read of 100 from 1e5 records returns 100, skips 60 (under the stride of 64) and reads 39,681 bytes (one chunk), and rescans nothing; the live heap of a
16-stream merge is 3,316,857 bytes at 1e4 records and at 1e5 (ratio 1.000); append and decode allocate 1 and 0 times per record.

## Open questions for the owner

1. **Push the first commit** (nothing is pushed; `core.hooksPath` is set to `.githooks` in this clone, the gate runs on push).
2. **Where Fabric's tag line starts**: Version.cmake gives `0.0.0` until a `v<major>.<minor>` tag. Squad and Canvas pin by commit until then.
3. **The `lines` framing has no checksum** (it is journal-1 as it is). Should Squad's W5 adoption move new journals to `frames` (checksums, header, rollover) and keep `lines` only to read
   old ones? F0 supports both; the choice is Squad's.
4. **Platform has no positional or streaming read**: the io layer uses `std::ifstream` for reads and Platform for append, truncate, sync, lock and atomic write. Grow Platform with a `readAt`
   (BACKLOG item 8), or leave reads in Fabric?
5. **Record `time` and `ts`**: canvas events carry a display `time` string, journal-1 a numeric `ts`; neither orders anything. Keep both shapes as they are (F0) or unify at W3.
6. **The journal facade keeps all records in memory** as Squad's does; offer a streaming `replay` for large journals when Squad adopts it?
7. The Sub0Log read-only adapter (review D3, W2) is **not in F0**: it needs Sub0Log's segment format, which this review pass did not read (the task named Log as a model only).

## What W3 (protocol and projection) needs from F0

* `Record` and `RecordCodec` as the unit of a protocol message: `kind@version`, writer, `seq` (position), payload as raw JSON: a projection can allowlist fields by reading the payload with simdjson.
* `Cursor` (`cursor-1`): the protocol's resume token, a per-writer position map with a stable text form; `since(cursor)` yields exactly the rest once, in the stream's order policy.
* `StoreReader::streams()` for the protocol's "what streams exist" query, and `ReadStats` for the K0 budgets.
* An in-process transport can be a `RecordSource`: `Merger` already is one, so a golden contract suite can run over `MemorySource`s with no files.
* Missing, to be added in W3: a versioned envelope for query and event messages (not the record's), and a place for the live ring (BACKLOG item 5).

## What W4 (HTTP+SSE and stdio transports) needs

* A live tail: `StoreReader::refresh()` is a directory scan plus an extension scan, cheap when nothing changed (it stats each segment), but a push transport wants a notification: either
  a writer-side callback (an in-process writer) or a poll interval; the one in-memory ring of recent records shared by every subscriber is not built.
* Cursor-addressed SSE events: the cursor's JSON is small and ordered, usable as an event id; `since` is the reconnect path.
* The `http` module home is Fabric `transport` (review D10); nothing of it exists yet. Platform's `net` has the sockets.
* The `Layout::shared` and `perWriter` choice and the lock matter for a server process that owns a writer: it holds the lock for its lifetime.
