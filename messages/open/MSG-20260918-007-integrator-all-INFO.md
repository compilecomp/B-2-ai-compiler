---
id: MSG-20260918-007
type: INFO
from: integrator
to:
  - all
severity: P2
status: OPEN
laws_refs:
  - Rule 5
  - Rule 7
  - Rule 14
  - Rule 15
  - Rule 40
  - Rule 42
  - Rule 96
  - Rule 122
  - Rule 124
  - Rule 132
  - Amendment B.1
  - Amendment B.3
related_prs: []
related_tests:
  - tests/codegen/T2CorpusTest.cpp
  - tests/interp/corpus/conversions.rbc
  - tests/interp/corpus/fields.rbc
  - tests/interp/corpus/float_math.rbc
  - tests/interp/corpus/strings_intern.rbc
created: 2026-09-18
---

## Summary

The partial deopt v0 contract lands in this commit, adding item 13
to `docs/STATUS.md`'s "Recommended next steps" list. The contract is
the design for the v1 layer that sits on top of the v0 method-
granularity invalidation (`docs/deopt_backend.md` Section 17): when
a T2 guard fails, the v0 does full method deopt to T0 (Rule 96
form, the existing path in
`compiler/codegen/src/Engine.cpp` `executeCompiled` lines 900-934);
the v1 attempts to salvage the speculation by rebuilding only the
smallest safe recompilation region containing the broken
assumption's dirty closure, keeping unaffected optimized code
alive.

The design principle is **production-safe by construction**:

> Partial deopt must never be required for correctness.
> It is only a performance salvage mechanism.
> If we can prove partial rebuild is safe, do it.
> If we cannot prove it, deopt fully to T0.

The v0 ships shadow-only: `enable_partial_deopt = false` is the
default; the engine's trap handler short-circuits every call to
`onGuardFailure()` and goes straight to the existing T0 deopt
path. The v0 → v1 transition implements the bodies; the v1
default flips to `true` once the shadow comparison (compute
partial plan, verify, do not activate, compare with full deopt
behavior) passes the corpus.

## What the IR already carries (the compile-time scaffolding)

The B-2 IR (`include/b2/ir/Graph.h`, `include/b2/ir/Node.h`)
already has the per-node metadata the partial deopt layer needs at
compile time. The v0 implementation work is the runtime structures
that consume this metadata, not the metadata itself:

| User's design field | B-2 IR field | Status |
|---|---|---|
| `assumptions[]` per node | `SpecMeta.dependency` (`DependencyId`); `Node.specMeta` (`SpecMetaId` + 1) | **exists** |
| `guards[]` per node | `NodeKind::Guard` with `payload = GuardKind`, `payload2 = DeoptId` | **exists** |
| `effect_class` per node | `EffectKind` (from `b2/ir/Effect.h`); `NodeInfo.effect` | **exists** |
| `region_id` per node | — | **MISSING** (the v0 work: a side table in the pipeline orchestrator) |
| `debug_info` for deopt reconstruction | `FrameStateDesc` (`method`, `pc`, `caller`, `vobjOffset`, `vobjCount`) | **exists** |
| Replacement log (Rule 14) | `ir::Replacement` (`oldNode`, `newNode`, `epoch`) | **exists** |
| Epoch tagging (Rule 14) | `Node.epoch`, `Graph::epoch()`, `Graph::nextEpoch()` | **exists** |
| Use-def chains (for dirty-node closure) | `Graph::usesOf(n)` returns `SmallVector<Use, 3>` | **exists** |
| Assumption key (`Dependency`) | `ir::Dependency` (`Kind ∈ {ClassHierarchy, MethodBody, FieldFinality, ProfileCounter, StaticProof}`, `target`) | **exists** |
| Invalidation API | `Graph::addDependency(dep)` returns `DependencyId` | **exists** |

What the IR does NOT have (the v0 work):

- A global `DependencyIndex` mapping `DependencyId → set<NodeId>`
  (the inverse map). The IR's `addDependency` returns an id but
  nothing in the runtime tracks which nodes depend on which
  assumption. The v0 contract surface for this is
  `include/b2/pipeline/DependencyIndex.h`.
- A `Region` structure (entry/exit control, values, memory state,
  exception exit, assumptions, guards, code handle, epoch). The v0
  contract surface is `include/b2/pipeline/Region.h`.
- The dirty-node closure algorithm + the region safety checks +
  the partial rebuild + the atomic swap machinery. The v0
  contract surface for the guard-failure integration is
  `include/b2/pipeline/PartialDeopt.h`.

## What lands here at v0 (contract surfaces only)

New files (passes-team area, applied by the integrator wearing the
passes-team hat per the `MSG-20260830-001` precedent — there are no
separate committers in the tree today):

- `docs/partial_deopt_contract.md` — the v0 contract, 25 sections
  mirroring the design:
  - Section 0: the core rule (never mutate the live optimized
    sea-of-nodes graph in place),
  - Section 1: what the IR already carries (the table above),
  - Section 2: the runtime `DependencyIndex` (the inverse map),
  - Section 3: the assumption keys (`ir::Dependency::Kind` taxonomy),
  - Section 4: the `Region` structure (the boundary contract),
  - Section 5: the dirty-node closure algorithm,
  - Section 6: the region safety checks (10 checks),
  - Section 7: copy-on-write (never patch live nodes),
  - Section 8: the boundary ABI (entry/exit state),
  - Section 9: the guard-failure flow (the integration point in
    `Engine.cpp` `executeCompiled`),
  - Section 10: the fallback strategy (5 escalation levels),
  - Section 11: the partial rebuild algorithm,
  - Section 12: what to preserve (unaffected optimized code),
  - Section 13: handling side effects (the hard part),
  - Section 14: handling memory state (sea-of-nodes memory edges),
  - Section 15: handling loops (OSR support required),
  - Section 16: handling inlined code (the ICDG boundary),
  - Section 17: activation must be atomic (epoch-based),
  - Section 18: deopt state is mandatory (the `FrameStateDesc`
    mapping),
  - Section 19: deopt budget and hysteresis (avoid deopt storms),
  - Section 20: verification gates (Rule 40 form),
  - Section 21: production safety policy (feature flags + shadow
    mode),
  - Section 22: telemetry,
  - Section 23: dangerous cases to reject immediately,
  - Section 24: recommended architecture,
  - Section 25: the final design principle.
- `include/b2/pipeline/DependencyIndex.h` — the runtime inverse
  map (`DependencyId → set<NodeId> / set<RegionId>`). Headers-only;
  every function is a no-op (returns empty sets). Defines
  `b2::pipeline::DirtySet` and `b2::pipeline::DependencyIndex`.
- `include/b2/pipeline/Region.h` — the recompilation region
  structure. Headers-only. Defines `ir::RegionId` (pipeline-owned;
  adding it to the IR would be a serialization format change the
  v0 contract explicitly defers), `b2::pipeline::Region` (the
  boundary contract: entry/exit control + values + memory +
  exception exit + assumptions + guards + code handle + epoch +
  telemetry), and `b2::pipeline::RegionSafety` (the 10-check
  safety verdict from Section 6).
- `include/b2/pipeline/PartialDeopt.h` — the guard-failure
  integration surface. Headers-only. Defines
  `b2::pipeline::PartialDeoptConfig` (the feature flags from
  Section 21; v0 defaults conservative),
  `b2::pipeline::PartialDeoptDecision` (`ActivatedNewRegion` /
  `EscalateToFullDeopt` / `Disabled` / `ShadowComputed`), and
  `b2::pipeline::onGuardFailure()` (the function the engine's trap
  handler calls; v0 returns `Disabled` for every call).
- `include/b2/pipeline/README.md` — the public API surface stub
  documentation.

Modified files:

- `compiler/pipeline/CMakeLists.txt` — declares `b2::pipeline` as
  an INTERFACE library linking `b2::ir` so consumers get the IR's
  include path transitively (the contract surfaces depend on the
  IR's public types: `ir::Graph`, `ir::Node`, `ir::DependencyId`,
  `ir::FrameStateDesc`, etc.).
- `compiler/pipeline/README.md` — updated from the v0 stub text to
  the v0 contract surface text (the directory now hosts the
  contract surfaces for partial deopt, not just a forward hook).
- `docs/deopt_backend.md` Section 17 — cross-references the
  partial deopt contract (the v0 method-granularity invalidation
  is the base; the v1 region-granularity partial deopt extends it).
- `.github/CODEOWNERS` — adds `docs/partial_deopt_contract.md` to
  the integrator-owned list.
- `docs/STATUS.md` — adds item 13 to "Recommended next steps"; adds
  the change-log entry; bumps the rev to 4.

Did NOT touch: `docs/laws.md`, `docs/stencils.md`,
`docs/cpp26_standards.md`, `docs/teams/*`,
`compiler/passes/src/*` (passes team's existing implementations),
`compiler/interp/src/*` (interpreter team), or
`compiler/codegen/src/*` (the T2 lowering and the engine's trap
handler — the v0 is shadow-only, no engine changes).

## Why this is shadow-only at v0

The v0 default `enable_partial_deopt = false` is conservative for
three reasons:

1. **The T2 driver (landed in `MSG-20260918-005`) has known
   divergences** (3 no-opt + 1 opt-mode, tracked as
   `MSG-20260918-006`). The partial deopt layer salvages T2
   speculation; if the T2 speculation itself is broken (the
   opt-mode `strings_intern.rbc` divergence is a passes+codegen
   contract bug), partial deopt would salvage broken code. The
   v0 waits for the T2 driver's v0 → v1 transition to fix the
   divergences before flipping the partial deopt default.
2. **The atomic swap (Section 17) needs the safepoint handshake
   protocol** (Rules 11, 13; `docs/STATUS.md` item 5). The v0
   `b2t2` is single-threaded; the v0 partial deopt would be
   synchronous (run in the calling thread, after the T0 fallback
   has resumed), which loses the perf salvage (the next
   invocation re-triggers the partial recompile). The v1 multi-
   threaded case is what unlocks the actual perf win.
3. **The shadow comparison (Section 21) is the validation tool**.
   The v0 computes the partial plan, verifies it, does NOT
   activate it, and compares with the full deopt behavior. The
   v1 default flips to `true` once the shadow comparison passes
   the corpus. The v0 ships with shadow mode on
   (`PartialDeoptConfig::shadow_mode = true`), but the partial
   deopt feature flag itself is off
   (`enable_partial_deopt = false`); the shadow computation runs
   only when the flag is on (so the v0 default is genuinely
   shadow-only, not just "shadow + activate").

## Evidence

- `git show --stat HEAD` on the commit that lands these changes.
- Local build + test sweep (pre-push):

  ```
  $ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -G Ninja
  $ cmake --build build -j
  $ ctest --test-dir build --output-on-failure
  100% tests passed out of 10
  Total Test time (real) = 0.06 sec
  ```

  No behavior change: the v0 is shadow-only, the engine's trap
  handler is unchanged, the T2 differential test
  (`tests/codegen/T2CorpusTest.cpp`) still passes 16/19 with the
  same 3 known no-opt bugs in `kKnownBugs`.

- Header compile check: the new headers under `include/b2/pipeline/`
  compile cleanly under `-Wall -Wextra -Wpedantic -Wshadow` (the
  repo's standard flags). They declare types and no-op inline
  functions only; no consumer links them yet (the engine's trap
  handler will be the first consumer in the v0 → v1 transition).

## Impact

- **passes**: the partial deopt layer lives in your team's
  `compiler/pipeline/` directory (the v0 stub landed in
  `MSG-20260918-004`). The contract surfaces under
  `include/b2/pipeline/` are the v0 → v1 transition's starting
  point. Your team is the authority on the dirty-node closure
  algorithm (Section 5), the region safety checks (Section 6), and
  the partial rebuild (Section 11); please review the contract and
  reply with corrections if the v0 → v1 plan is off. The hard
  dependency is the multi-threaded compilation base
  (`docs/STATUS.md` item 5); the v0 → v1 transition cannot ship
  the atomic swap (Section 17) without it.
- **codegen**: the guard-failure integration point is your team's
  `compiler/codegen/src/Engine.cpp` `executeCompiled` (lines
  900-934 today). The v0 does NOT modify this path; the v0 → v1
  transition adds a call to `b2::pipeline::onGuardFailure()`
  between the guard-failure detection and the T0 fallback. Your
  team is the authority on the trap handler's behavior; please
  review Section 9 of the contract and reply with corrections.
- **ir**: the partial deopt layer consumes your team's
  `ir::Dependency`, `ir::SpecMeta.dependency`, and
  `ir::FrameStateDesc` (all already in the IR). The v0 contract
  surfaces define `ir::RegionId` in `include/b2/pipeline/Region.h`
  (pipeline-owned; adding it to the IR would be a serialization
  format change the v0 contract explicitly defers). Your team is
  the authority on the IR's serialization format; please review
  Section 1 of the contract and reply with corrections if the
  "what the IR already carries" table is inaccurate.
- **interpreter**: the partial deopt layer's fallback (Section 10)
  is your team's T0 path. The v0 escalates to full method deopt
  (the existing path); the v1 attempts partial rebuild first. Your
  team is the authority on the T0 deopt path's behavior; please
  review Sections 9 and 10 of the contract and reply with
  corrections.
- **regalloc / aot**: no direct impact. The partial deopt layer's
  v1 may consume your team's v1 outputs (the new region's
  GC-reference liveness for the safepoint stack map; the new
  region's spill/reload edits under the MIR contract), but that's
  the v0 → v1 transition's coordination, not this v0 contract.
- **integrator / governance**: `docs/STATUS.md` item 13 is added
  to "Recommended next steps"; the change-log entry lands; the v0
  contract is the honest design for what the partial deopt layer
  WILL do when the v0 → v1 transition ships.

## Requested Action

No immediate code change is required from any team. Read the
files that affect your team and reply with corrections:

- **passes**: read `docs/partial_deopt_contract.md` Sections 2, 5,
  6, 11, 17, 19, 20, 22 (the algorithmic heart of the partial
  deopt layer). Verify the v0 → v1 plan is right. Read
  `include/b2/pipeline/DependencyIndex.h`, `Region.h`,
  `PartialDeopt.h` and verify the type definitions and APIs are
  right.
- **codegen**: read Section 9 of the contract and verify the
  guard-failure integration plan is right (the call to
  `onGuardFailure()` between the guard-failure detection and the
  T0 fallback).
- **ir**: read Section 1 of the contract and verify the "what the
  IR already carries" table is right.
- **interpreter**: read Sections 9 and 10 of the contract and
  verify the fallback ladder is right.
- **all**: read the contract's Section 25 (the final design
  principle). The v0 ships shadow-only; the v1 default flips to
  `true` once the shadow comparison passes the corpus.

## Boundaries

The integrator will not modify any team's `compiler/`, `include/`,
or `tests/` write list beyond:

- the new contract surfaces under `include/b2/pipeline/`
  (`DependencyIndex.h`, `Region.h`, `PartialDeopt.h`, `README.md`)
  — passes-team area, applied by the integrator wearing the
  passes-team hat per the `MSG-20260830-001` precedent;
- the `compiler/pipeline/CMakeLists.txt` (passes-team area) to
  link `b2::ir` transitively;
- the `compiler/pipeline/README.md` (passes-team area) to update
  from the v0 stub text to the v0 contract surface text;
- `docs/partial_deopt_contract.md` (integrator-authored;
  cross-team contract for what the partial deopt layer will do
  when the v0 → v1 transition ships);
- `docs/deopt_backend.md` Section 17 cross-reference (integrator-
  authored; the cross-team deopt/backend contract);
- `docs/STATUS.md` (integrator-owned living document);
- `.github/CODEOWNERS` (integrator-owned);
- `messages/open/MSG-20260918-007-...INFO.md` (this message).

The integrator will not modify `docs/laws.md`,
`docs/stencils.md`, `docs/cpp26_standards.md`, `docs/teams/*`,
`compiler/passes/src/*` (passes team's existing implementations),
`compiler/interp/src/*` (interpreter team), or
`compiler/codegen/src/*` (the T2 lowering and the engine's trap
handler — the v0 is shadow-only, no engine changes).

The new contract surfaces under `include/b2/pipeline/` live at
the paths the ownership map assigns to the passes team
(`compiler/pipeline/` is listed at `passes.write`); the
integrator is landing them because the passes team has not yet
written them and the v0 → v1 transition needs a stable surface
to target. Once landed, the passes team owns the headers; the
integrator will not modify them further without team approval.

## Response

```text
status:
responder:
date:
notes:
```
