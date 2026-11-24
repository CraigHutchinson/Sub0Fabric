# Design

Status: F0, the log core (W2 of the architecture review). The decisions D1-D11 and the section 5 budgets are the review's, in Sub0Canvas
`docs/reviews/2026-11-23-fabric-portal-store.md`; this document says what F0 built from them, the byte formats, and where it differs. It does not copy the review.

## Scope of F0

| review | F0 |
|---|---|
| D1 lift Squad's journal behaviour-preserving; Canvas's events the second user | done: `journal-1` and `canvas-event-1` are the two proved record shapes; Squad's journal tests are ported (`test_journal1`) |
| D2 modules `segment`, `record`, `order`, `cursor`, `store` | done as pure headers `segment`, `record`/`codec`, `order`, `cursor`, `index`, `layout`, and the io directory store; `transport` and `projection` are W3/W4 |
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
