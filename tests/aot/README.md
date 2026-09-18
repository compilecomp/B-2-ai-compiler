# `tests/aot/` — AOT team tests (v0 stub)

**Status:** v0 stub. **No tests.**

This directory exists so `docs/teams/ownership.yaml` is consistent with
the tree. The aot team's tests will live here when v1 lands; until then
the directory holds only this README and the `CMakeLists.txt` stub.

## What lands here at v1

When the aot team ships the offline compiler, this directory will hold:

- offline driver tests (closed-world analysis, reachable-methods
  enumeration),
- manifest emission + mechanically-checked proofs (Amendment B.5 form),
- AOT object file format (`.b2o`) tests,
- AOT link/loader tests (relocation, symbol resolution, deopt backend
  wiring),
- golden stress fixtures (large programs, deep type hierarchies,
  reflective code that must be reachable from the closed-world analysis),
- differential tests vs the T0 interpreter and T1 baseline JIT
  (byte-identical output across tiers, the Rule 36 form).

The test harness will follow the `tests/rbc/CMakeLists.txt` pattern: a
`b2_aot_tests` executable that links `b2::aot`, an `add_test()` entry,
and a `corpus/` subdirectory of fixtures.

## See also

- `docs/aot_contract.md` — v0 contract (forward contract only)
- `docs/teams/aot-team.md` — team charter
- `compiler/aot/README.md` — aot team implementation stub
