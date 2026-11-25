# Design

Status: F0, the log core (W2 of the architecture review), and the query protocol (W3a, "Protocol (W3a)" below). The decisions D1-D11 and the section 5 budgets are the review's, in Sub0Canvas
`docs/reviews/2026-11-23-fabric-portal-store.md`; this document says what F0 built from them, the byte formats, and where it differs. It does not copy the review.

## Scope of F0

| review | F0 |
|---|---|
| D1 lift Squad's journal behaviour-preserving; Canvas's events the second user | done: `journal-1` and `canvas-event-1` are the two proved record shapes; Squad's journal tests are ported (`test_journal1`) |
| D2 modules `segment`, `record`, `order`, `cursor`, `store` | done as pure headers `segment`, `record`/`codec`, `order`, `cursor`, `index`, `layout`, and the io directory store; `transport` is W4, `projection` is W3b; the protocol is W3a ("Protocol (W3a)") |
| D4 order is data | done: the order policy is a field of the segment header and a name resolved by `findOrderPolicy` (`seq`, `lamport`); `aligned-time` waits for the Sub0Log adapter |
| D5 physical per writer, logical per project | done: a writer owns its segment directory; "per project" is the reader's merge across writers; `Layout::shared` is the single-stream option |
| D6 composite writer id | done: `WriterId{ns, writer}` is in the record, the cursor, the header and the path |
| D3 Sub0Log read-only adapter | **not in F0** (BACKLOG item 3): the seam it needs (`RecordCodec`, `OrderPolicy`) is in place |
| section 5 budgets for append, `since`, merge, parse | benchmarks and deterministic metrics (below); git-ref backend, live fan-out, projection and indexes-per-user are later waves |

## Layers

`pure` (`sub0fabric::pure`) does no I/O, reads no clock, starts no thread and reads no environment; simdjson stays in the sources. `io` (`sub0fabric::io`)
includes pure and Sub0Platform and nothing else. `cmake/CheckLayers.cmake` (test `layering`) fails an upward include, a simdjson include in a public
header and any I/O or clock header in pure.

## Record and shapes

`Record` is one neutral struct (`kind`, `writer`, `seq`, `lamport`, `ts`, `subject`, `time`, `payload`, `extra`). A **shape** (`RecordCodec`) says which of
those members its wire form carries, and encodes and decodes one JSON object. Two are proved:

| shape | wire fields | order | writer in the record | `seq` in the record |
|---|---|---|---|---|
| `journal-1` (Sub0Squad) | `seq`, `ts`, `type`, `d` | `seq` | no (the stream's) | yes, checked against the position |
| `canvas-event-1` (Sub0Canvas, BACKBONE.md section 3.3, A22) | `kind`, `writer{namespace,writer}`, `lamport`, `subject`, `time`, `payload`, then unknown members | `lamport` | yes | no: the position is the segment's |

The field sets are **disjoint** (`fieldSetsDisjoint`, held by `test_codec`), so `detectShape` tells a line's shape from its fields and a text with both sets is an
error. That answers the review's unverified question (section 3): the journal and the canvas envelope do not collide on `seq` or `type`.

* `payload` (journal `d`) is raw JSON kept verbatim, so a reader that does not know a kind never rewrites it. Canvas's unknown envelope members are kept in `extra`
  and written back byte for byte. journal-1 ignores unknown members, as Squad's reader does.
* `seq` is the **1-based position in the writer's stream**, set by the segment on read and on append for every shape. It is the cursor's unit.
* The writer helper and the parser pool in `src/pure/json_util.hpp` repeat Sub0Canvas's idiom, because Fabric depends on no other Sub0 project (a candidate to
  lift into Sub0Platform if a third consumer wants it).

## Segment byte formats

A segment is the bytes one writer appended. Two framings, over any shape:

### `lines`: the journal-1 file, unchanged

One record per `\n`-terminated line, no header, no checksum, file name `journal-000001.ndjson`. Byte-identical to what Sub0Squad's `Journal` writes. Rules, as
Squad's `Journal::replay`:

* no newline at the end: torn tail (the partial line is dropped);
* a last line that does not parse, or whose `seq` is not the position it sits at: torn tail;
* the same fault on any earlier line: **corrupt**, reported with the byte offset, never skipped.

Integrity rests on the parse, as in Squad: a parser that does not read a value (it only slices the raw `d`) does not notice damage inside it. The `frames`
framing is the one with checksums. A record whose text holds a newline is refused (it would split the line).

### `frames`: the native segment, `seg-NNNNNN.s0f`

```
segment  := header frame*
header   := "S0FSEG1\n"  u32 bodyLen  body  u32 crc32c(bodyLen bytes + body)       little-endian integers
body     := {"format":"fabric-seg-1","shape":"canvas-event-1","order":"lamport","writer":{"namespace":"..","writer":".."},"first":N}
frame    := u32 len  u32 crc32c(len bytes)  payload[len]  u32 crc32c(payload)       12 bytes of overhead per record
```

* The **header is data**: the shape and the order policy are the stream's (review D4), the writer id is the composite (D6), `first` is the 0-based position of
  the segment's first record, so a rolled segment continues the count and a reader finds the segment holding position p from headers alone.
* CRC-32C (Castagnoli), slice-by-8, pure and the same on every machine (`test_segment` holds the standard check value).
* The **length has its own checksum**. A crash leaves a clean prefix of what was written, so a complete 8-byte header that fails its own checksum is damage,
  while an incomplete payload behind an intact header is a tear. That is what lets a torn tail be cut with confidence and a damaged length not be mistaken for one.
* A length over 64 MiB is corruption (a damaged length cannot make a reader allocate without bound).

**Torn-tail rule** (the scanner, both framings): an incomplete or bad record that is the last thing in the segment is `torn` and `validOffset()` says where to
cut; damage before the end is `corrupt`; a tail of zero bytes (a file extended and not written, which some file systems leave) is torn, and data after
zeros is corrupt. A frame passing its checksum but not decoding is corrupt, even last: the checksum proves it was fully written.

### The scanner

`SegmentScanner` takes bytes in pieces (`feed`, `finish`) and yields records: memory is the piece in hand plus the largest frame, never the segment
(`test_segment` feeds one byte at a time and holds the buffer to the piece plus a frame). With a null `out` it only checks (frames: checksum, no decode), which
is the open-for-append recovery and the index build. It reports `offset()` before each record for the sparse index and `reset()` to seek.

## Order, merge, cursor

* **Order policy** (`order.hpp`) is a name to a total order: `seq` = (seq, writer); `lamport` = (lamport, writer, seq) (review O3). A new policy is one table row.
* **`Merger`** is a streaming k-way merge over `RecordSource`s: one head record per source in a binary heap, O(log k) per record, memory bounded in the number
  of sources, never of records. A source out of its own order is counted (`outOfOrder`), not fatal; a source error stops the merge and is returned.
* **`Cursor`** (`cursor-1`) is one position per writer (records consumed = the `seq` of the last), not one number for the merged stream, so a record that arrives
  late below an advanced position is still delivered exactly once (the fix BACKBONE.md section 3.4 names for B2's lamport cursor). Text form: ordered, so
  equal cursors give equal bytes; stored atomically by the io layer.
* **`SparseIndex`**: the offset of every 64th record per segment; `since` seeks to the nearest entry and decodes at most 63 records it does not return.

## Directory store

```
<root>/<ns>/<writer>/seg-000001.s0f, seg-000002.s0f, ...     Layout::perWriter (default): ns and writer percent-encoded, empty = "~"
<root>/seg-000001.s0f                                          Layout::shared: the root is one stream, one process at a time
<dir>/journal-000001.ndjson                                    the lines framing, in either place
<dir>/.lock                                                    held by an open writer (WriterOptions::lock)
```

**Writer** (`StoreWriter`): one writer owns a stream, so append takes no lock between writers and is O(1): the record is encoded once into a reused buffer and
written with one `AppendFile::append`. Durability is a policy (`perRecord`, `perBatch` default, `interval`); `appendBatch` is one write per segment and one
flush, the unit later transports and the git-ref store commit by. Opening **recovers**: the active segment is scanned in chunks (frames: checksums only, no decode),
a torn tail is truncated, an untouched torn header is rewritten (its `first` taken from the segment before it), and damage before the tail refuses the open
(an append would bury it). The segment rolls at `maxSegmentBytes` (frames only; a lines file is one segment, as journal-1 is); the next header's `first` continues the
count. A second writer on the same stream is refused by the lock, which is how "one writer per segment" is enforced rather than assumed.
`Layout::shared` is the review's per-attempt/per-project option (D5, off by default): one stream in the root for one process at a time.

**Reader** (`StoreReader`): `open` finds every stream (root, then `<ns>/<writer>`), reads each segment header, and scans each segment once, building its sparse
index and count; `refresh` finds new segments and extends the scan from where it stopped (an unchanged file is not touched). `since(cursor)` builds one
`StreamSource` per stream with unread records (the plan is copied, so it survives a refresh), seeks each to the nearest index entry at or before the cursor, and
returns a `Merger`. Memory: one chunk (64 KiB) per stream and one head record per stream, however long the logs; records per read: those returned, plus at most
`indexEvery - 1` skipped per stream (`ReadStats` counts them, `test_store` and the benchmarks check it as a fact). A stream that stops early is an error naming the file
(a segment shorter than its scan), and damage found by the scan is reported by `streams()` and by the read when it reaches it, after the records before it.
Streams with different order policies are an error until one is named (`orderOverride`); a `lines` stream's shape is found from its first line by `detectShape`.

**Cursors** (`io/cursor_file.hpp`): `loadCursor`/`saveCursor`, atomic replace through Platform's `writeFileAtomic`. A reader advances its cursor only after it
has handled records, so a crash repeats work and never loses it.

**journal-1 facade** (`io/journal1.hpp`): Sub0Squad's `Journal` API over the same machinery (shared layout, lines framing, shape `journal-1`, no lock, as Squad's).

### How journal-1 is preserved

1. **The bytes**: `lines` framing with the `journal-1` shape writes exactly Squad's line. `tests/fixtures/journal-1/squad-written.ndjson` was written by Squad's own
   `Journal`; `test_journal1` reads it, writes every record back to identical bytes (the `ts` doubles included: both sides use the same simdjson), and continues it.
2. **The behaviour**: `test_journal1` runs Squad's `test_journal.cpp` with only the include and namespace changed: truncation at every byte (replay never fails and returns a
   prefix; open cuts the tail; the next append continues the sequence), corruption in the middle is an error, with Squad's message text.
3. **The rules**: no newline at the end is torn; a bad or wrong-`seq` last line is torn; the same before the end is corrupt; a missing journal replays empty. These are the scanner's
   rules for the lines framing, so they hold for any shape written as lines.
4. **Not preserved, deliberately**: `d` is kept without trailing whitespace; the facade keeps every record in memory, as Squad's does (the reader is the streaming way);
   a lines record whose text holds a newline is refused instead of silently splitting (Squad never had such a body: its bodies are compact JSON).

## Protocol (W3a)

`fabric-query-1` is the one query protocol every transport serves (review D7: protocol first, independent of transport). It is **pure** (`pure/protocol.hpp`): a
JSON request text in, a JSON response text out, over a `QuerySource`; no transport, no I/O, no clock. This section is the single home of the schema.

```
RequestHandler     handle(request, RequestContext) -> response          pure/handler.hpp   the seam every transport calls
  ProtocolServer   info | streams | since                               pure/protocol.hpp  the fabric-query-1 handler (W3b adds a projecting one)
QuerySource        streams() ; since(cursor, order) -> RecordSource     pure/query_source.hpp
  MemoryQuerySource                                                     pure/query_source.hpp   reference, tests
  StoreQuerySource                                                      io/store_query_source.hpp   over StoreReader
serveLines(handler, input, context)                                     pure/lines.hpp     JSON lines: one response line per request line
```

**Request**: one JSON object. `op` is required; `schema` (if present) must be `"fabric-query-1"`; `id` (a string or a whole number) is echoed as the raw token it came as
(a `uint64` id survives byte for byte); anything else as `id` is `bad-request`. The caller's identity is not a request member: the transport passes a `RequestContext`
(`actor`, `capabilities`) to the handler (W3a answers every caller alike; W3b's projecting handler is what reads it).

**Success**: `{"schema":"fabric-query-1","ok":true,"op":..,"id":..,"at":{"streams":N,"records":M},...payload...,"unknownFields":[..]}`. `at` is the position the answer was
computed at (streams and records the source held), so a client sees whether anything moved between answers. `id` appears only when the request had one; `unknownFields` only in
`echo` mode when there is something to list.

**Error**: `{"schema":"fabric-query-1","ok":false[,"op":..][,"id":..],"error":{"code":..,"field":..,"message":..}}`; `op` and `id` are echoed when they were well formed in the
request, `field` is the request member at fault (empty for the request as a whole). Codes: `bad-request`, `unsupported-schema`, `unknown-op`, `unknown-field` (policy
`reject`), `bad-cursor`, `too-large` (request over `maxRequestBytes`, checked before the parse), `mixed-order` (streams of different order policies and no `order` named),
`store-error` (a damaged or unreadable stream; the message names the file). The checks run in a fixed order whatever order the members came in: size, JSON, `id`, `schema`,
`op`, the op's own members, unknown members, then the work.

| op | request members | payload |
|---|---|---|
| `info` | | `ops`, `policy` (as `parseProtocolPolicy` reads it), `shapes` (from `codecs()`), `orders` (from `orderPolicies()`) |
| `streams` | `limit`, `cursor` | `entries` each `{"namespace","writer","shape","order","records"[,"damage"]}` ordered by writer; `next`: the last writer sent as `{"namespace","writer"}`, a place and not an offset, `null` on the last page |
| `since` | `cursor`, `limit`, `order` | `entries` each `{"writer":{"namespace","writer"},"seq":N,"shape":S,"record":<the codec's encode bytes, verbatim>}`; `next` always a `cursor-1` object (the request's cursor advanced past every returned record, or unchanged when nothing is new, so a caught-up client keeps it and asks again); `more` (found by pulling one record beyond the page) |

`limit` is a positive whole number; absent it is the policy default, above the policy maximum it is clamped and the response carries `"clamped":true`. `cursor` for `since` is the
raw `cursor-1` object (absent or `null`: from the start). `order` names an order policy that overrides the streams' own (`seq`, `lamport`); an unknown name is `bad-request`
field `order`. A member an op does not take is an unknown member, even when another op takes it.

**Bounds**: a page is at most `limit` entries, and stops adding entries once its entries reach `maxResponseBytes` (the first entry is always added, so one entry over the cap goes
out alone; the bound is the cap plus the last entry plus the small envelope). A damaged stream does not hide what precedes the damage: the page that reaches it returns the records
before it with `more:true`, and the next request, which starts at the damage, is the `store-error`.

**Unknown members** (`unknownFields` in the policy): `ignore` (answered as if absent, byte for byte), `echo` (the default: answered, and the names listed in request order, at most
16, each clipped to 64 characters), `reject` (`unknown-field` naming the first). Echo is the forward-compatible default: a newer client sees what an older server dropped.

**The policy is data**: `ProtocolPolicy` (`streams` and `since` page sizes `{default,max}`, `maxResponseBytes`, `maxRequestBytes`, `unknownFields`), read from JSON by
`parseProtocolPolicy` (every member optional; an unknown member, a zero size or a default above its maximum is an error naming the member) and reported by `info`.

### Contract (fabric-query-1)

| # | property | held by |
|---|---|---|
| P-1 | every failure is a structured error (never an exception, never an empty reply), with the exact bytes of each code | `test_protocol` `envelopeErrors`, `idEcho`, `unknownFields`, `policyRoundtrip`; golden cases; `test_protocol_store` damage |
| P-2 | `since` pages give every record exactly once, per writer, in policy order, including a late writer's lower-lamport record | `test_protocol` `sinceExactlyOnce` (limits 1..n against one full `Merger` drain), `sinceLateWriter`, `sinceOrder`; `test_protocol_store` append then refresh |
| P-3 | a response is bounded by `limit` and by `maxResponseBytes` plus one entry | `test_protocol` `sinceBounds`, `streamsPaging` (clamping); golden `maxResponseBytes` case |
| P-4 | records come out in wire form verbatim: the codec's encode bytes, Canvas's unknown members and the payload untouched, a journal-1 entry carrying its stream's writer | `test_protocol` `entriesVerbatim`; golden `canvas` and `journal` cases |
| P-5 | the same input gives the same bytes on every source and compiler | `test_protocol` `determinism` and the golden cases; `test_protocol_store` golden cases byte for byte over files (the expected bytes are fixed in `cases.txt`, so every compiler in the CI matrix is held to them) |
| P-6 | cursors are places: a `streams` cursor is the last writer sent, a `since` cursor is per-writer positions, so a page never repeats or skips what changed between requests | `test_protocol` `streamsPaging`, `sinceLateWriter`, `sinceExactlyOnce` |
| P-7 | identity reaches the handler through `RequestContext`, never from the request: a request member naming an actor is an unknown member and changes nothing | `test_protocol` `unknownFields` (the `actor` block), the `RequestHandler` signature |

Determinism rules the code keeps (so P-5 holds on gcc, clang and MSVC): simdjson on-demand reads members in document order; ordered containers only (`std::map`, never a hash
order); no clock, no random; and the source pull and the cursor advance are separate statements, never two side effects in one argument list or operator chain (gcc evaluates
right to left, clang left to right).

### The judgement call: a shared contract, not shared code

Sub0Canvas's `query-1` (its `docs/QUERY.md` sections 1, 3 and 6) defined the envelope first: `schema`, `ok`, `op`, `id`, `error{code,field,message}`, `limit`/`clamped`, `next`,
`maxResponseBytes` with the first entry always added, and the three `unknownFields` modes. Fabric cannot link Canvas (Canvas depends on Fabric, never the reverse, and
Fabric depends on no other Sub0 project), and a query over a document differs from a query over a log. So Fabric **takes over the contract** under its own schema name,
`fabric-query-1`, and writes its own implementation; a client that speaks one envelope speaks the other, and a divergence is a deliberate edit of a documented table, not drift in
two copies of code. The cost is two small parsers that must stay alike, which the golden cases here and Canvas's tests each hold on their side. The same reasoning put
`json::Writer` here as a repeated idiom (see "Record and shapes").

## Budgets (review section 5) and how F0 measures them

`bench/` builds `Sub0Fabric_Bench` (sub0perf). Deterministic metrics in `perf/metrics.json` run in every tier; timing is local only (`ctest --preset timing`).

| review budget | F0 evidence |
|---|---|
| append: O(1), no lock, durability a policy | allocations per append the same at 1e3 and 1e5 records (metric); bytes per record (metric); timing rows for append at each durability mode |
| `since(cursor)`: O(records returned) via a sparse index, never a rescan | records skipped and bytes read for a tail read of a 1e5-record log (metrics: skipped below the stride, bytes at most a chunk); scan counters do not move on a second read; timing: tail read at 1e4 and 1e5 |
| merge: streaming k-way, memory bounded in segments not records | live heap bytes of a 16-stream merge of 1e5 records equals that of 1e4 within a stride of headroom (metrics); timing: merge throughput |
| parse: simdjson on-demand, one JSON library | decode allocations per record (metric); timing: decode MB/s for both shapes |
| git-ref backend, live fan-out, projection, indexes per user | later waves (W4, W6) |

## Open points

See [BACKLOG.md](BACKLOG.md) and RESUME.md "Open questions for the owner".
