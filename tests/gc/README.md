# `tests/gc/` — GC team tests (v0 stub)

**Status:** v0 stub. **No tests.**

This directory exists so `docs/teams/ownership.yaml` is consistent with
the tree. The gc team's tests will live here when v1 lands; until then
the directory holds only this README and the `CMakeLists.txt` stub.

## What lands here at v1

When the gc team ships the collector, this directory will hold:

- region-allocation unit tests (alloc, free, region boundaries),
- write-barrier correctness tests (Germ Merging write-back to remembered
  sets, SATB snapshot barriers),
- concurrent marker + safepoint handshake tests,
- evacuator + promoted-pinned-region tests,
- golden stress fixtures (heap growth under load, allocation churn,
  pinning storms, reference-queue liveness),
- the verifier's correctness suite against the IR verifier's
  operand-type checks (Rule 124 determinism).

The test harness will follow the `tests/rbc/CMakeLists.txt` pattern: a
`b2_gc_tests` executable that links `b2::gc`, an `add_test()` entry, and
a `corpus/` subdirectory of fixtures.

## See also

- `docs/gc.md` — Generational Concurrent Region-Based Collector
- `docs/teams/gc-team.md` — team charter
- `compiler/gc/README.md` — gc team implementation stub
