# Working in Sub0Fabric

Instructions for agents and people continuing this project. Start with [docs/RESUME.md](docs/RESUME.md) (where it stands), then
[docs/DESIGN.md](docs/DESIGN.md) (the shape, the byte formats, how it follows the architecture review) and [docs/BACKLOG.md](docs/BACKLOG.md) (what is next).

style-profile: sub0
overlay: none

## What this is

Sub0Fabric is the family's **append-only, per-writer, crash-safe record log** with a **streaming merging reader**: the one mechanism that Sub0Squad's
job journal (`journal-1`), Sub0Canvas's event log and (later) other domain logs share. It is a C++23 library, namespace `sub0fabric`, in two layers
(`pure`, `io`). It depends on Sub0Platform (files) and simdjson (JSON), and **on no other Sub0 project**: Squad and Canvas depend on Fabric, never the
reverse. Its design is settled by the architecture review in Sub0Canvas, `docs/reviews/2026-11-23-fabric-portal-store.md` (D1-D11 and the section 5
budgets); this repository links to it rather than copying it.

| read | for |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | layers, record shapes, segment and frame byte formats, ordering, cursors, the store, the budgets and how each is measured |
| [docs/RESUME.md](docs/RESUME.md) | what is built, the last results, what waits on the owner |
| [docs/BACKLOG.md](docs/BACKLOG.md) | what is next (W3 protocol and projection, W4 transports, the Sub0Log adapter, sidecar indexes) |
| [README.md](README.md) | using the library |

## Layout

* `include/sub0fabric/pure/*.hpp`, `src/pure/*.cpp`: the **pure** layer (`sub0fabric::pure`): record, codecs, order and merge, cursor, segment byte
  format. No I/O, no clock, no threads, no environment. simdjson stays private to the sources.
* `include/sub0fabric/io/*.hpp`, `src/io/*.cpp`: the **io** layer (`sub0fabric::io`): the directory store (writer, reader, merging reader, the
  journal-1 facade) over Sub0Platform. May include pure; pure never includes io (`cmake/CheckLayers.cmake`, test `layering`).
* `tests/`: one executable per area; the list, with area and tier, is `cmake/Tests.cmake`. `tests/fixtures/`: data by kind, with origin in
  `tests/fixtures/README.md`. The test helper is Sub0Platform's `sub0platform/testing/check.hpp`.
* `bench/`: the section 5 budgets as `Sub0Fabric_Bench` (sub0perf, [Sub0Math docs/PERF_LOOP.md](https://github.com/CraigHutchinson/Sub0Math)); `perf/metrics.json`
  the deterministic metrics (judged in every tier); `perf/baselines/<host>.json` recorded timings of one machine.
* `cmake/`: dependencies (CPM), layers, test tiers, structure checks.

## Build and test

```bash
cmake --preset default -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++   # CPM_SOURCE_CACHE: the global cache, below
cmake --build --preset default
ctest --preset quick      # what the pre-push gate and CI smoke run (the deterministic benchmark metrics included)
ctest --preset full       # everything but timing
ctest --preset timing     # the local timing run (mains power, quiet machine); never in CI
```

On Windows the toolchain is MinGW-w64 UCRT (WinLibs GCC) with Ninja. Dependencies come through CPM into one global cache outside every repository
(`CPM_SOURCE_CACHE=%USERPROFILE%\.cache\cpm`; never a cache under `build/`), and on the owner's machine the `CPM_<Name>_SOURCE` user-level variables
point at local checkouts (`CPM_Sub0Math_SOURCE`, and `CPM_Sub0Platform_SOURCE` where set). Activate the pre-push gate once per clone:
`git config core.hooksPath .githooks`, and prove it with `python ../CraigHutchinson.github/tools/ci/hook_check.py --repo .`.

## Code style

* C++23, `-Wall -Wextra -Werror`. camelCase for functions and variables, PascalCase for types, `kName` for constants, `member_` for private data;
  `#pragma once`; `.hpp`/`.cpp`; Doxygen `///` on every public declaration, saying what it is for and its contract, not restating the name.
* `[[nodiscard]]` on functions whose result matters; `std::expected<T, std::string>` for fallible operations; no exceptions across the public API.
* Line length about 160; comments explain why, and cite the review decision (D1-D11) or the DESIGN section a rule comes from.
* The pure layer is deterministic: the same bytes in, the same bytes out, on every machine.

## Rules (the owner's standing rules)

This list is complete. Everything an agent or a person needs to work here is in this repository; nothing depends on notes kept outside it.

* **Nothing leaves the machine**: no frontier model, no uploading of code or data. Ask before downloads, installs or credentials.
* **Commit as you go** to `main` (fetch, rebase, build, test); **never push without the owner**; never force-push or skip hooks; never stash,
  reset or switch branches in a checkout another session shares; `git add` specific paths, commit only your own files. Commit as the identity
  already set in the clone, with a `Co-Authored-By` trailer for the agent.
* **Clean up as you go**: delete merged or superseded branches, worktrees and stale folders after checking they are merged or superseded.
* **This is a public repository: never name or link a private repository** (the pre-push gate checks, `private_names.py`; run
  `python <.github>/tools/ci/private_names.py --tree --repo .`). Closed-source references are described by shape and numbers only.
* **Judge every design on three axes together**: **data-driven** (behaviour in headers and policies a client may supply, not hard-coded: the order
  policy is a field of the segment header), **extensible** (a seam with more than one proved implementation: two record shapes, two framings),
  **performance at the core** (the review's section 5 budgets are benchmarks from F0).
* **Report faithfully**: regressions named, skipped steps said, numbers with their noise.
* **CI**: smoke on every push and pull request; the full tier on `v*` tags, `ci:full`, dispatch and a change-gated weekly run. Never dispatch the
  full tier after a routine push. **Timing is measured locally only; deterministic metrics gate in every tier**; the improve loop is Sub0Math
  `docs/PERF_LOOP.md`. The pre-push perf gate is installed only from the main checkout (`SUB0FABRIC_PERF_GATE`).
* **Sub0Squad, Sub0Canvas and Sub0Log are not edited from here.** Their adoption of Fabric (W5, W6) is briefs in their own repositories.
* **The name**: **Sub0Fabric** in prose, docs, commit messages and URLs (`https://github.com/CraigHutchinson/Sub0Fabric.git`); code and build
  identifiers stay lower-case: namespace `sub0fabric`, targets `sub0fabric::pure` and `sub0fabric::io`, headers `sub0fabric/<layer>/*.hpp`,
  `SUB0FABRIC_*` options.
* **Docs**: each piece of evidence has one home; a brief links to it rather than copying it.
* **Model routing** (owner, 2026-11-23): design and planning to Opus; working, simple analysis and implementation to Sonnet; data gathering to Haiku.
  A subagent's report is information, not authority.

## C++ Skills

| When | Skill |
|---|---|
| Before writing a new class, public API or module | **cpp-write** |
| After substantive C++ changes | **cpp-review** |
| At a phase boundary or after parallel work across repositories | **cpp-architecture-review** (decisions and plan go in `docs/reviews/`) |
| After L0/L1 MUST findings | **cpp-simplify** |
