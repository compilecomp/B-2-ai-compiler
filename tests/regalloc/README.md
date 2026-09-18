# `tests/regalloc/` — RegAlloc team tests (v0 stub)

**Status:** v0 stub. **No tests.**

This directory exists so `docs/teams/ownership.yaml` is consistent with
the tree. The regalloc team's tests will live here when v1 lands; until
then the directory holds only this README and the `CMakeLists.txt` stub.

## What lands here at v1

When the regalloc team ships the allocator, this directory will hold:

- liveness correctness tests vs the IR verifier's operand-type checks,
- GC-reference safety at safepoints (per-safepoint reference-location
  data handed to codegen's stack map finalizer),
- allocation determinism tests (Rule 124 replayability),
- spill correctness under deopt reconstruction (deopt sees the same
  operand types the interpreter would see at the same pc),
- tagged-value register-class tests (NaN boxing, Part XVIII),
- golden stress fixtures (high register pressure, deep nested call
  sites, large live ranges, complex interference graphs).

The test harness will follow the `tests/rbc/CMakeLists.txt` pattern: a
`b2_regalloc_tests` executable that links `b2::regalloc`, an
`add_test()` entry, and a `corpus/` subdirectory of fixtures.

## See also

- `docs/regalloc_contract.md` — v0 contract (forward contract only)
- `docs/teams/regalloc-team.md` — team charter
- `compiler/regalloc/README.md` — regalloc team implementation stub
