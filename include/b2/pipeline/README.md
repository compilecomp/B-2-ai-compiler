# `include/b2/pipeline/` — Pipeline team public API (v0 contract surface)

**Status:** v0 contract surface — type definitions only, **no implementation**.
See `docs/partial_deopt_contract.md` and `docs/STATUS.md`.

This directory holds the public header surface for the pipeline
orchestrator's partial deopt layer. The IR (`include/b2/ir/`) already
carries the compile-time scaffolding (`ir::Dependency`,
`ir::SpecMeta.dependency`, `ir::FrameStateDesc`, `ir::Replacement`,
`ir::Graph::addDependency()`); the headers here are the runtime
structures that CONSUME that scaffolding:

- `DependencyIndex.h` — the runtime inverse map
  `DependencyId → set<NodeId> / set<RegionId>` (Section 2 of the
  contract). The IR's `addDependency` returns an id but nothing in
  the runtime tracks which nodes depend on which assumption; the
  `DependencyIndex` is the inverse map the runtime needs to mark
  nodes dirty when an assumption breaks.
- `Region.h` — the recompilation region structure (Section 4 of
  the contract). Defines `ir::RegionId` (pipeline-owned; adding
  it to the IR would be a serialization format change the v0
  contract explicitly defers), `Region` (the boundary contract:
  entry/exit control + values + memory + exception exit +
  assumptions + guards + code handle + epoch + telemetry), and
  `RegionSafety` (the 10-check safety verdict from Section 6).
- `PartialDeopt.h` — the guard-failure integration surface
  (Section 9 of the contract). The engine's trap handler in
  `compiler/codegen/src/Engine.cpp` `executeCompiled` calls
  `onGuardFailure()` after detecting a guard failure; the function
  decides whether to attempt partial rebuild or escalate to full
  method deopt.

## Why headers-only

The v0 ships shadow-only (Section 21 of the contract): the API is
declared but every function is a no-op (returns `Disabled` /
empty sets). The v0 default `enable_partial_deopt = false`
short-circuits every call. The v0 → v1 transition implements the
bodies; until then, callers (the T2 driver's guard-failure path)
fall through to the existing T0 deopt path (the engine's trap
handler, unchanged).

The header-only form means consumers (`compiler/codegen/`) can
`#include <b2/pipeline/PartialDeopt.h>` without changing the build
graph; the v0 → v1 transition adds `.cpp` files under
`compiler/pipeline/` when the implementation lands.

## What lands here at v1

When the partial deopt layer graduates to v1, this directory will
also hold:

- `DirtyClosure.h` — the dirty-node closure algorithm (Section 5).
- `RegionSafety.h` — the safety check predicates (Section 6).
- `PartialRecompile.h` — the partial rebuild (Section 11).
- `Activation.h` — the epoch-based atomic swap (Section 17).
- `Telemetry.h` — the partial-deopt counters (Section 22).

All v1 residents will be headers + `.cpp` files (the v1 needs
real implementations, not just contract surfaces).

## See also

- `docs/partial_deopt_contract.md` — the v0 contract (the design
  this header surface implements)
- `docs/deopt_backend.md` — the v0 deopt system (the
  method-granularity invalidation this contract extends)
- `docs/t2_driver_contract.md` — the T2 driver (the consumer of
  partial deopt)
- `compiler/pipeline/README.md` — the pipeline team implementation
  stub (the v0 → v1 transition's home)
