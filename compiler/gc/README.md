# `compiler/gc/` — GC team (v0 stub)

**Status:** v0 stub. **No implementation.** See `docs/STATUS.md` and
`docs/gc.md`.

This directory exists so `docs/teams/ownership.yaml` is consistent with
the tree: the gc team's write paths (`compiler/gc/`, `include/b2/gc/`,
`tests/gc/`) all resolve to real directories. The actual garbage
collector is design-only today — the interpreter uses a bump allocator
with handles (`compiler/interp/src/Heap.cpp`, 312 lines), no
generational collector, no write barriers, no concurrent marking.

## What lands here at v1

When the gc team ships, this directory will hold the generational
concurrent region-based collector described in `docs/gc.md`:

- the region allocator with `MemRegion` and `RegionTable`,
- write-barrier fast paths ( Germ Merging write-back to remembered sets),
- the snapshot-at-the-beginning write barrier for SATB marking,
- the concurrent marker thread + safepoint handshake protocol,
- the evacuator with promoted-pinned-region support,
- the verifier + golden tests under `tests/gc/`.

The v0 -> v1 transition goes through the message system per
`docs/teams/messaging.md` and requires an ADVISORY to every affected
team (interpreter, baseline_noir, codegen, ir, passes, aot) before any
collector code lands.

## Non-guarantees (v0)

- No allocation is performed by `b2_gc`. The interpreter's bump allocator
  remains the only allocator.
- No write barriers, no remembered sets, no safepoint handshake.
- The `b2::gc` CMake target is an INTERFACE library today; linking
  against it pulls in no objects.

## See also

- `docs/gc.md` — Generational Concurrent Region-Based Collector (design)
- `docs/teams/gc-team.md` — team charter
- `docs/STATUS.md` — open-work entry on real GC
- `include/b2/gc/README.md` — public API surface (also a stub)
