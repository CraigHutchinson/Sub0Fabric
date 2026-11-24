# Sub0Fabric

An append-only, per-writer, crash-safe **record log** for C++23 with a **streaming, merging reader**. One writer owns each segment, so appending needs no
lock; a reader merges every writer's segments into one deterministic order without loading them into memory; a torn tail left by a crash is cut off on
open and never loses an earlier record. Records are JSON, framed with checksums (or as plain NDJSON lines for compatibility), and carry a `kind@version`
so a reader that does not know a kind never rewrites it.

Status: **F0, the log core** (pure layer and directory store). The protocol and projection (W3) and the transports (W4) are next; see
[docs/BACKLOG.md](docs/BACKLOG.md). Design: [docs/DESIGN.md](docs/DESIGN.md).

## What is in F0

| layer | contents |
|---|---|
| `sub0fabric::pure` | record and shapes (`journal-1`, `canvas-event-1`), CRC-32C, frame and line framing, segment header, torn-tail scanner, sparse offset index, order policies and the streaming k-way merge, durable cursors |
| `sub0fabric::io` | directory store: per-writer segment files, appending writer with a durability policy, merging reader (`since(cursor)`), the `journal-1` facade with Sub0Squad's `Journal` API |

## Use

```cpp
#include "sub0fabric/io/store.hpp"

auto writer = sub0fabric::io::StoreWriter::open(root, {"project", "w-000001"}, {.shape = "canvas-event-1"});
sub0fabric::Record r;
r.kind = "item.put@1"; r.lamport = 1; r.payload = R"({"key":"a"})";
(void)writer->append(r);

auto reader = sub0fabric::io::StoreReader::open(root);
sub0fabric::Cursor cursor;                       // empty: from the start
auto stream = reader->since(cursor);             // merged in (lamport, writer) order, streaming
for (sub0fabric::Record rec; stream->next(rec);) { /* ... */ cursor.advance(rec); }
```

## Build

```bash
cmake --preset default -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build --preset default
ctest --preset quick
```

Dependencies (Sub0Platform, simdjson) are fetched with CPM. See [AGENTS.md](AGENTS.md) for the toolchain, the tiers and the rules.

## Licence

MIT, see [LICENSE](LICENSE).
