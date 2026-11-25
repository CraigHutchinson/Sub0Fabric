# Where Sub0Fabric stands

Status: living. Rewritten at the end of each working session. Last: 2026-11-24 (F0 built, then W3a, the query protocol, on branch `w3a-protocol`; nothing pushed, nothing tagged).

Read [DESIGN.md](DESIGN.md) for the shape, the byte formats and the protocol ("Protocol (W3a)", the single home of the schema), [BACKLOG.md](BACKLOG.md) for what is next. The decisions are
the architecture review's (Sub0Canvas `docs/reviews/2026-11-23-fabric-portal-store.md`, D1-D11 and section 5); this repository is its waves W2 and W3.

## Built

| part | state | evidence |
|---|---|---|
| scaffold: layers `pure` and `io`, CPM, Version.cmake, test tiers, presets, CI (smoke, full), `.ci/local.json`, pre-push gate, `SUB0FABRIC_PERF_GATE` | built | `layering`, `test_reporting`, `hook_check.py` (5 pass), `local_ci.py --level quick` |
| record shapes `journal-1` and `canvas-event-1` with disjoint field sets, detection by fields | built | `test_codec`; fixtures written by Squad's own `Journal` and Canvas's own `eventJson` read and written back byte for byte |
| segment formats: `lines` (the journal-1 file) and `frames` (header, length-checked, CRC-32C), torn-tail and corruption rules, zero tails, incremental scanner | built | `test_segment`: truncation at every byte for both framings, chunked feed at every piece size |
| order policies as data (`seq`, `lamport`), streaming k-way merge, per-writer cursors, sparse index | built | `test_order` (a cursor position above INT64_MAX now round-trips) |
| directory store: writer (recovery, rollover, durability policy, lock), merging reader (`since(cursor)`, `refresh`), cursor files, per-writer and shared layouts | built | `test_store`: crash at every byte, damage before the tail, rollover, index seeks as counted facts |
| journal-1 facade with Squad's `Journal` API; Squad's journal test ported | built | `test_journal1` |
| section 5 budgets as `Sub0Fabric_Bench`: 31 deterministic metrics (every tier), 10 timing rows, first baseline | built | `perf/metrics.json`, `perf/baselines/croglegion.json` (untouched by W3a) |
| **W3a** the `fabric-query-1` protocol: `RequestHandler`/`RequestContext` (identity from the transport), `QuerySource` with `MemoryQuerySource` and `StoreQuerySource`, `ProtocolServer` (`info`, `streams`, `since`), `ProtocolPolicy` as data, `serveLines` (JSON lines), `json::Writer` arrays and unsigned numbers | built | `test_protocol` (16 blocks, one per property P-1..P-7 and per seam), the golden suite `tests/fixtures/fabric-query-1` (33 cases, every code a read-only source reaches), `test_protocol_store` (the same cases over files, byte-identical; damage; append, refresh, read from the old `next`) |

`ctest --preset quick` and `--preset full`: 12 of 12 pass (GCC 16, MinGW-w64 UCRT, Ninja). The slow ones are `test_store` and `test_journal1` (truncation at every byte); the two protocol tests
take well under a second each. Clang 22 builds Fabric's own code clean (checked earlier, with Platform's `strerror` deprecation silenced) but fails inside Sub0Platform's `file.cpp` on this
machine, as Sub0Canvas's optional clang step does; W3a was built with GCC only. No timing run was made for W3a, and no existing byte moved: `journal-1`, `canvas-event-1` and `cursor-1`
text, the segment and frame formats, the existing fixtures, `perf/metrics.json` and `perf/baselines` are unchanged (`git diff origin/main` over those paths is empty).

## Measured (first baseline, `croglegion`, mains power, Balanced, 5 rounds, CV under 4 %; F0, not re-measured)

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

1. **Push** (nothing is pushed; `core.hooksPath` is `.githooks` in the clone, the gate runs on push). W3a is six commits on `w3a-protocol`.
2. **Where Fabric's tag line starts**: Version.cmake gives `0.0.0` until a `v<major>.<minor>` tag. Squad and Canvas pin by commit until then.
3. **The `lines` framing has no checksum** (it is journal-1 as it is). Should Squad's W5 adoption move new journals to `frames` and keep `lines` only to read old ones?
4. **Platform has no positional or streaming read**: the io layer uses `std::ifstream` for reads (BACKLOG item 8).
5. **Record `time` and `ts`**: neither orders anything. Keep both shapes as they are or unify at W3b.
6. **The journal facade keeps all records in memory** as Squad's does; offer a streaming `replay` when Squad adopts it?
7. The Sub0Log read-only adapter (review D3) is not built: it needs Sub0Log's segment format.
8. **W3a judgement calls to confirm**: (a) the envelope contract is Canvas's `query-1`, taken over under the schema name `fabric-query-1` with Fabric's own code (DESIGN.md, "The judgement
   call"); (b) a page is cut once its entries reach `maxResponseBytes` after adding one (as Canvas), not before; (c) a damaged stream returns the records before the damage and then the
   `store-error` on the next request, rather than failing the whole page; (d) the canvas codec still casts `lamport` through a signed number (a lamport above INT64_MAX would be written
   wrong; no real clock gets there, and changing it is a `canvas-event-1` byte question for the owner).

## What W3b (projection-1) needs from W3a

* `RequestContext` is in the handler seam already (`actor`, `capabilities`) and no handler reads it yet: the projecting handler wraps `ProtocolServer` as a second `RequestHandler`.
* `QuerySource` is the place to filter streams (the listing must show only allowed writers), and `since` entries carry `record` as the codec's verbatim bytes: a projection replaces `record`
  with a `payload` built from an allowlist, reading the payload with simdjson, and rewrites writer ids and cursors to opaque ids. See BACKLOG item 1b.
* The golden harness (`tests/golden.hpp`, `cases.txt` with `@@@ source` and `@@@ policy` directives) is reusable: a projection suite is another cases file over the same sources.

## What W4 (HTTP+SSE and stdio transports) needs

* `serveLines` is the stdio transport's whole logic; a process around it only reads and writes bytes. A line over `maxRequestBytes` is answered `too-large` by the handler, but a transport
  must still bound what it buffers while looking for the newline.
* A live tail: `StoreReader::refresh()` is cheap when nothing changed (it stats each segment), but a push transport wants a notification or a poll interval; the in-memory ring of recent
  records shared by every subscriber is not built. `StoreQuerySource` sees what the reader has scanned, so the owner calls `refresh()` before a request that should see new records.
* Cursor-addressed SSE events: the `since` cursor is small and ordered, usable as an event id; `since` is the reconnect path.
* The `http` module home is Fabric `transport` (review D10); nothing of it exists yet. Platform's `net` has the sockets.
* The `Layout::shared` and `perWriter` choice and the lock matter for a server process that owns a writer: it holds the lock for its lifetime.
