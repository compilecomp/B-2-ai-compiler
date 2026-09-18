# `compiler/pipeline/` — passes team (v0 contract surface)

**Status:** v0 contract surface. **Headers-only;** no `.cpp` files.
See `docs/partial_deopt_contract.md` and `docs/STATUS.md`.

This directory is the passes team's home for the pipeline
orchestrator: the runtime structures that order passes, manage
budgets, lower IR to machine code (the T2 driver is the consumer
end of this), and orchestrate the partial deopt layer. The
v0 → v1 transition items are documented in
`docs/partial_deopt_contract.md`.

## What landed here at v0

The v0 ships contract surfaces only (headers, no implementations):

- `include/b2/pipeline/DependencyIndex.h` — the runtime inverse
  map `DependencyId → set<NodeId> / set<RegionId>` (Section 2 of
  the contract). The IR's `addDependency` returns an id; the
  `DependencyIndex` is the inverse map the runtime needs to mark
  nodes dirty when an assumption breaks.
- `include/b2/pipeline/Region.h` — the recompilation region
  structure (Section 4 of the contract). Defines `ir::RegionId`,
  `Region` (the boundary contract), and `RegionSafety` (the
  10-check safety verdict from Section 6).
- `include/b2/pipeline/PartialDeopt.h` — the guard-failure
  integration surface (Section 9 of the contract). The engine's
  trap handler in `compiler/codegen/src/Engine.cpp`
  `executeCompiled` calls `onGuardFailure()` after detecting a
  guard failure.

The v0 ships shadow-only: every function is a no-op (returns
`Disabled` / empty sets). The v0 default
`enable_partial_deopt = false` short-circuits every call. The
v0 → v1 transition implements the bodies; until then, callers
fall through to the existing T0 deopt path.

## Hard dependencies (cannot ship until)

- **T2 execution driver** — landed in `MSG-20260918-005` (`b2t2`,
  `compiler/codegen/tools/b2t2.cpp`). The partial deopt layer
  salvages T2 speculation; without T2, there's nothing to salvage.
  Today the T2 driver is wired end-to-end with a 16/19 differential
  (3 known no-opt divergences + 1 opt-mode divergence tracked as
  `MSG-20260918-006`); the partial deopt layer is the v0 → v1
  transition's response to the opt-mode divergences (the current
  `b2t2 -O` produces no output for `strings_intern.rbc` because
  the SCCP-folded graph breaks the T2 lowering; partial deopt
  would catch the broken speculation at the guard site, escalate
  to T0 for the broken region, and preserve the optimized code
  outside the broken region).
- **Multi-threaded compilation** (`docs/STATUS.md` item 5) — the
  partial deopt layer's atomic swap (Section 17) needs the
  safepoint handshake protocol (Rules 11, 13). Today `b2t2` is
  single-threaded; the v0 ships shadow-only, the v1 needs the
  multi-threaded base.

## What lands here at v1

When the partial deopt layer graduates to v1, this directory
will hold:

- `src/DependencyIndex.cpp` — the inverse map implementation.
- `src/Region.cpp` — the region builder + partitioner.
- `src/DirtyClosure.cpp` — the dirty-node closure algorithm
  (Section 5 of the contract).
- `src/RegionSafety.cpp` — the safety check predicates
  (Section 6).
- `src/PartialRecompile.cpp` — the partial rebuild (Section 11).
- `src/Activation.cpp` — the epoch-based atomic swap (Section 17).
- `src/Telemetry.cpp` — the partial-deopt counters (Section 22).
- `src/PartialDeopt.cpp` — the guard-failure integration (the
  `onGuardFailure` body, Section 9).

And additional header surfaces:

- `include/b2/pipeline/DirtyClosure.h`
- `include/b2/pipeline/RegionSafety.h`
- `include/b2/pipeline/PartialRecompile.h`
- `include/b2/pipeline/Activation.h`
- `include/b2/pipeline/Telemetry.h`

The v0 → v1 transition goes through the message system per
`docs/teams/messaging.md`; the v1 ships with an INFO message and
an accompanying ADVISORY to every affected team (interpreter,
baseline_noir, ir, passes, codegen, regalloc, aot, gc/runtime)
before the v1 code lands.

## Non-guarantees (v0)

- No partial deopt is performed. The v0 default
  `enable_partial_deopt = false` short-circuits every call to
  `onGuardFailure`; the engine's trap handler goes straight to
  the existing T0 deopt path (the v0 method-granularity
  invalidation, `docs/deopt_backend.md` Section 17).
- The `b2::pipeline` CMake target is an INTERFACE library today;
  linking against it pulls in no objects. The v0 → v1 transition
  adds the `.cpp` files when the implementations land.

## See also

- `docs/partial_deopt_contract.md` — the v0 contract (the design
  this directory implements)
- `docs/deopt_backend.md` — the v0 deopt system (the
  method-granularity invalidation this contract extends)
- `docs/t2_driver_contract.md` — the T2 driver (the consumer)
- `docs/pass_contracts.md` — pass contracts
- `docs/special_passes.md` — special pass designs (CM-PEA, effect
  reordering, adaptive value representation)
- `docs/icdg.md` — inline call/dispatch graph contract
- `docs/STATUS.md` — open-work entry on the partial deopt layer
- `docs/teams/passes-team.md` — team charter
- `include/b2/pipeline/README.md` — the public API surface stub
