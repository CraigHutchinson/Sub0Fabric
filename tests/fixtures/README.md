# Fixtures

| path | kind | origin | checked by | refresh |
|---|---|---|---|---|
| `journal-1/squad-written.ndjson` | captured | four records written by Sub0Squad's own `Journal` (`journal.cpp` as of its 2026-11-23 head), including an escaped quote, a newline escape, a non-ASCII character, a 14-digit integer and an exponent number; the wall-clock `ts` values are whatever the capture saw | `test_journal1`: Fabric reads it, and writes every record back to the same bytes | write a small program against Squad's `Journal::append` and copy `journal-000001.ndjson` here; the `ts` values change, the test does not mind |
