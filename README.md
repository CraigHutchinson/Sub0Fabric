# Sub0Fabric

An append-only, per-writer, crash-safe **record log** for C++23 with a **streaming, merging reader**. One writer owns each segment, so appending needs no
lock; a reader merges every writer's segments into one deterministic order without loading them into memory; a torn tail left by a crash is cut off on
open and never loses an earlier record. Records are JSON, framed with checksums (or as plain NDJSON lines for compatibility), and carry a `kind@version`
so a reader that does not know a kind never rewrites it.

Status: **F0, the log core** (pure layer and directory store) and **W3a, the query protocol** (`fabric-query-1`). Projection (W3b) and the transports (W4) are next; see
[docs/BACKLOG.md](docs/BACKLOG.md). Design: [docs/DESIGN.md](docs/DESIGN.md).

## What is in F0

| layer | contents |
|---|---|
| `sub0fabric::pure` | record and shapes (`journal-1`, `canvas-event-1`), CRC-32C, frame and line framing, segment header, torn-tail scanner, sparse offset index, order policies and the streaming k-way merge, durable cursors |
| `sub0fabric::io` | directory store: per-writer segment files, appending writer with a durability policy, merging reader (`since(cursor)`), the `journal-1` facade with Sub0Squad's `Journal` API |
| `sub0fabric::pure` protocol | `RequestHandler` (one request text in, one response text out, with the transport's `RequestContext`), `ProtocolServer` (`fabric-query-1`: `info`, `streams`, `since`), `QuerySource` seam, `serveLines` (JSON lines) |
| `sub0fabric::io` protocol | `StoreQuerySource`: the directory store as a query source |

## Use

```cpp
#include "sub0fabric/io/store.hpp"

auto writer = sub0fabric::io::StoreWriter::open(root, {"project", "w-000001"});   // canvas-event-1, frames, one segment per writer
sub0fabric::Record r;
r.kind = "item.put@1"; r.lamport = 1; r.payload = R"({"key":"a"})";
(void)writer->append(r);

auto reader = sub0fabric::io::StoreReader::open(root);
sub0fabric::Cursor cursor;                       // empty: from the start
auto stream = reader->since(cursor);             // merged in (lamport, writer) order, streaming
for (sub0fabric::Record rec; ;) {
    auto got = (*stream)->next(rec);             // true: a record; false: the end; an error: damage, named
    if (!got || !*got) break;
    cursor.advance(rec);                         // then sub0fabric::io::saveCursor(file, cursor)
}
```

## Serve a query

```cpp
#include "sub0fabric/io/store_query_source.hpp"
#include "sub0fabric/pure/lines.hpp"
#include "sub0fabric/pure/protocol.hpp"

auto reader = sub0fabric::io::StoreReader::open(root);
sub0fabric::io::StoreQuerySource source(*reader);       // or sub0fabric::MemoryQuerySource
sub0fabric::ProtocolServer server(source);               // ProtocolPolicy is data: parseProtocolPolicy(json)
sub0fabric::RequestContext who;                          // the transport's identity of the caller, never from the request

std::string one = server.handle(R"({"op":"since","limit":100})", who);
// {"schema":"fabric-query-1","ok":true,"op":"since","at":{..},"entries":[..],"next":{"format":"cursor-1",..},"more":false}
// ask again with "cursor": <the next object> to read exactly what follows; "more" says whether a page is full

std::string lines = sub0fabric::serveLines(server, R"({"op":"info"}
{"op":"streams"}
)", who);   // one response line per request line
```

The schema, the error codes and the contract are in [docs/DESIGN.md](docs/DESIGN.md), "Protocol (W3a)".

## Build

```bash
cmake --preset default -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build --preset default
ctest --preset quick
```

Dependencies (Sub0Platform, simdjson) are fetched with CPM. See [AGENTS.md](AGENTS.md) for the toolchain, the tiers and the rules.

## Licence

MIT, see [LICENSE](LICENSE).
