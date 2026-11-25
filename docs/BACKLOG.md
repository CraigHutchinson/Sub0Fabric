# Backlog

What is next, in order. The wave numbers are those of the architecture review (Sub0Canvas `docs/reviews/2026-11-23-fabric-portal-store.md`, section 6).

| # | item | wave | note |
|---|---|---|---|
| 1a | Protocol: the versioned query protocol `fabric-query-1` (`info`, `streams`, `since`), the query-source seam over memory and the directory store, JSON-lines framing, golden contract suite | W3a | **done** (see RESUME.md, DESIGN.md "Protocol (W3a)") |
| 1b | Projection: `projection-1`, a field-allowlist `Projection` handler wrapping `ProtocolServer`. Writer ids become opaque, FNV-1a 64 of salt + namespace + `/` + writer; the projected `payload` (built from the allowlist) replaces `record`; `streams` lists only allowed writers; projected cursors carry the opaque ids. Reads `RequestContext` (capabilities) for who may see what; golden cases in the same harness (`tests/golden.hpp`) | W3b | **next**: needs `RequestHandler`, `QuerySource`, the golden harness (all in W3a) |
| 2 | Transports: stdio JSON lines and native HTTP+SSE (the `http` module, review D10), K0 budgets | W4 | needs the in-memory live ring and `StoreReader::refresh` (below) |
| 3 | The read-only Sub0Log codec adapter (review D3): a Sub0Log segment read through the record seam, order policy `aligned-time` | W2 remainder | not in F0: the adapter is the third codec; the seam (`RecordCodec`, `OrderPolicy` registry) is ready |
| 4 | Sidecar sparse index, a disposable per-user cache keyed by segment id (review A19), so a cold `since(cursor)` does not scan the segment | F1 | F0 builds the index in memory on first open and extends it as the segment grows |
| 5 | Live fan-out: one in-memory ring of recent records shared by every subscriber, cursor-addressed | W4 | review section 5 |
| 6 | Rollover for the NDJSON framing (the journal-1 file is one segment; the frame framing rolls over at `maxSegmentBytes`) | F1 | needs a decision on how a line segment names its first position |
| 7 | Store contract suite shared with the git-ref, git-notes and SQLite proofs of concept (review D9) | W6 | the contract is: append, `since(cursor)`, atomic batch |
| 8 | Platform gap: a streaming, positional read (`readAt`) in Sub0Platform; the io layer uses `std::ifstream` for reads today | - | owner decides whether Platform grows it |
| 9 | Record views (`RecordView` with string_views into the frame buffer) for the zero-copy read path | F1 | F0 copies the strings into `Record`; the parse is already simdjson on-demand |
