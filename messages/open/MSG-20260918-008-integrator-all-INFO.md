---
id: MSG-20260918-008
type: INFO
from: integrator
to:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 7
  - Rule 14
  - Rule 15
  - Rule 19
  - Rule 40
  - Rule 96
  - Rule 121
  - Rule 124
  - Amendment B.1
related_prs: []
related_tests:
  - tests/pipeline/TestHarness.h
  - tests/pipeline/TestMain.cpp
  - tests/pipeline/DirtyClosureTests.cpp
  - tests/pipeline/CMakeLists.txt
created: 2026-09-18
---

## Summary

The dirty-node closure algorithm (Section 5 of
`docs/partial_deopt_contract.md`) lands in this commit, graduating the
partial deopt layer from v0 (contract surfaces only, headers-only,
every function a no-op) to v0.1 (forward closure implemented + unit
tested). The algorithm is the input to:

- the region safety checks (Section 6: the minimal safe region contains
  the closure),
- the partial rebuild (Section 11: the new subgraph replaces the
  closure's nodes),
- the deopt budget (Section 19: if the closure exceeds
  `max_dirty_region_size`, the caller escalates to full method deopt).

The v0.1 is **forward closure only**. The backward closure (region
expansion through boundary nodes, Section 5's second loop) requires
region tracking, which doesn't exist yet (the v0 contract surfaces for
`Region` landed in `MSG-20260918-007`; the region BUILDER is the
v0 → v1 transition's work). The v0.1 returns the forward closure; the
v0 → v1 transition adds the backward closure when the region builder
lands.

## What lands here at v0.1

New files (passes-team area, applied by the integrator wearing the
passes-team hat per the `MSG-20260830-001` precedent — there are no
separate committers in the tree today):

- `include/b2/pipeline/DirtyClosure.h` — the v0.1 contract surface.
  Declares `b2::pipeline::DirtyClosureResult` (the dirty set + the
  `budget_exceeded` flag + the iteration-count telemetry),
  `b2::pipeline::kDefaultMaxDirtyRegionSize` (the default budget,
  default 64), `b2::pipeline::expandDirtyClosure()` (the algorithm),
  and `b2::pipeline::semanticsDependOn()` (the predicate, exposed for
  unit tests + the v0 → v1 transition's relaxation).
- `compiler/pipeline/src/DirtyClosure.cpp` — the implementation. A
  monotone fixpoint over the IR's def-use chains; deterministic
  (Rule 124) via sorted-vector membership + node-id-order iteration.
- `tests/pipeline/TestHarness.h` — the self-registration test harness
  (mirrors `tests/{frontend,rbc,ir,passes,interp,baseline,codegen}/
  TestHarness.h` exactly; same `B2_TEST` semantics, same registry/
  failure model).
- `tests/pipeline/TestMain.cpp` — the test driver.
- `tests/pipeline/DirtyClosureTests.cpp` — 12 unit tests:
  1. empty seed → empty closure,
  2. duplicate seeds → deduplicated dirty set,
  3. dead seed dropped (the closure does not propagate through dead
     nodes),
  4. forward closure through `InputRole::Data` (ConstantI → AddI →
     AddI),
  5. value with no users does not propagate (closure is the seed
     alone),
  6. memory-state chain propagation (ClassInit → StoreField →
     LoadField via `InputRole::Mem`),
  7. `InputRole::Parent` propagation (If → IfTrue projection),
  8. `InputRole::Ctrl` does NOT propagate (a Region with Ctrl
     predecessors does not become dirty from its Ctrl inputs),
  9. budget cap enforcement (`max_size=1` → `budget_exceeded = true`),
  10. determinism (Rule 124: identical graphs + seeds produce
      identical dirty sets across runs),
  11. `semanticsDependOn` for `InputRole::Data` slots,
  12. `semanticsDependOn` for side-effecting users (StoreField
      propagates dirty from ALL input slots, regardless of role).
- `tests/pipeline/CMakeLists.txt` — wires the test executable
  (`b2_pipeline_tests`) + registers it as the `pipeline_tests` ctest
  target.
- `messages/open/MSG-20260918-008-...INFO.md` — this announcement.

Modified files:

- `compiler/pipeline/CMakeLists.txt` — `b2_pipeline` is now a STATIC
  library (was INTERFACE in v0); adds `src/DirtyClosure.cpp` to the
  sources; the `pipeline_compile_check` target links `b2::pipeline`
  transitively (was linking `b2::codegen` only).
- `CMakeLists.txt` (top-level) — adds `add_subdirectory(tests/pipeline)`
  to the `B2_BUILD_TESTS` block.
- `docs/partial_deopt_contract.md` Section 5 — updated to reflect the
  v0.1 implementation status (forward closure implemented; backward
  closure open work; the predicate's exact rules; the budget cap; the
  unit tests; the determinism property).
- `docs/STATUS.md` — item 13 updated to v0.1; the change-log entry
  added; the rev bumped to 5.

Did NOT touch: `docs/laws.md`, `docs/deopt_backend.md`,
`docs/stencils.md`, `docs/cpp26_standards.md`, `docs/teams/*`,
`compiler/passes/src/*` (passes team's existing implementations),
`compiler/interp/src/*` (interpreter team), or
`compiler/codegen/src/*` (the T2 lowering and the engine's trap
handler — the v0.1 is shadow-only, no engine changes).

## The `semanticsDependOn` predicate (Section 5 of the contract)

The predicate is the conservative default. The implementation:

1. Dead users can't propagate dirty (they're tombstones; the verifier
   rejects dangling inputs to dead nodes anyway).
2. Side-effecting users (`NodeClass::Call`, `NodeClass::Memory` with
   non-`Pure` effect, `NodeClass::Guard`) ALWAYS depend on the
   producer regardless of the input slot's role — the side effect may
   observe the producer's value or speculation (Section 13: "If a
   dirty region contains calls/stores/allocations, be conservative").
3. Non-side-effecting users (`NodeClass::Value`, `NodeClass::State`,
   `NodeClass::TypeOp`, `NodeClass::Control`) depend on the producer
   ONLY through the input slot's role:
   - `InputRole::Mem` — the memory state chain. If the producer's
     speculation is invalidated, the memory state is invalidated.
   - `InputRole::Data` — the user's value depends on the producer's
     value.
   - `InputRole::FrameState` — the deopt state's locals depend on
     the producer's value (the deopt reconstruction needs the
     producer's value or its speculation's corrected value).
   - `InputRole::Parent` — projection source (IfTrue/IfFalse of an
     If, SwitchCase of a Switch, CallExcept of a Call*. The
     projection's behavior is determined by its Parent).
   - `InputRole::Ctrl` — control predecessor; does NOT propagate
     (the control token already flowed; the predecessor's identity
     doesn't change the projection's semantics).
   - `InputRole::None` — placeholder / unused slot; does NOT
     propagate.

The slot's role comes from the IR's `NodeInfo` registry
(`b2/ir/Node.h`): `roles[6]` for the fixed prefix, `variadicRole`
for slots beyond `numFixed`, and `FrameState` for the mandatory
trailing slot when `hasFrameState` is true.

## Evidence

- `git show --stat HEAD` on the commit that lands these changes.
- Local build + test sweep (pre-push):

  ```
  $ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -G Ninja
  $ cmake --build build -j
  $ ctest --test-dir build --output-on-failure
  100% tests passed out of 12
  Total Test time (real) = 0.06 sec
  ```

  The new `pipeline_tests` is the 12th ctest target (was 11 in
  `MSG-20260918-007`). All 12 tests in `tests/pipeline/
  DirtyClosureTests.cpp` PASS.

- ASan + UBSan build + test sweep (pre-push):

  ```
  $ cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14 \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -g" -G Ninja
  $ cmake --build build-san -j
  $ ctest --test-dir build-san --output-on-failure
  100% tests passed out of 12
  Total Test time (real) = 1.57 sec
  ```

  The v0.1 algorithm is clean under ASan + UBSan (no leaks, no UB;
  the sorted-vector membership + node-id-order iteration is the
  deterministic path; the closure's monotone fixpoint has no aliasing
  or use-after-free hazards).

- Regression sweep (no behavior change):

  ```
  $ b2jit --stats sweep over tests/interp/corpus/*.rbc: 19/19 T1 corpus green
  $ b2t2 --stats tests/interp/corpus/sum_loop.rbc:
    5050
    [b2t2] lowered=1 refused=0 attempts=0 ok=1 deopts(trap=1 callExc=0 guard=0) ...
  $ ctest -R codegen_tests: PASS (16/19 T2 differential, same 3 known bugs)
  ```

  The v0.1 is shadow-only; the engine's trap handler is unchanged;
  no behavior change across the T1/T2 paths.

## Impact

- **passes**: the dirty-node closure algorithm lands in your team's
  `compiler/pipeline/` directory. The implementation is the v0.1
  forward closure; the backward closure (Section 5's second loop)
  is open work that requires the region builder. Your team is the
  authority on the `semanticsDependOn` predicate (Section 5); the
  v0.1 is conservative (side-effecting users always propagate; pure
  value nodes propagate only through Data/Mem/FrameState/Parent
  roles). Please review the predicate's rules and reply with
  corrections if the conservative default is too aggressive (or not
  aggressive enough) for any NodeClass / InputRole combination.
- **codegen**: no impact. The v0.1 is shadow-only; the engine's
  trap handler in `compiler/codegen/src/Engine.cpp`
  `executeCompiled` is unchanged. The v0 → v1 transition adds the
  call to `b2::pipeline::onGuardFailure()` (the v0 contract surface
  from `MSG-20260918-007`) between the guard-failure detection and
  the T0 fallback. The v0.1's `expandDirtyClosure()` is called from
  `onGuardFailure()` in the v1; today it's only called from the
  unit tests.
- **ir**: no impact. The v0.1 implementation consumes the IR's
  public API only (`Graph::usesOf()`, `Graph::node()`,
  `Graph::nodeCount()`, `Node::isDead()`, `info()` from
  `b2/ir/Node.h`). The IR's `NodeInfo` registry is the source of
  truth for the `InputRole` of each slot; the predicate consults it
  via `roleOfSlot()` (an internal helper).
- **interpreter**: no impact. The v0.1 does not touch the runtime.
- **regalloc / aot**: no direct impact. The v0.1 is the first
  algorithmic piece of the partial deopt layer; your team's v1
  work (the MIR contract, the T2-pipeline-driven AOT) will consume
  the partial deopt layer once it's wired into the engine (the v0 →
  v1 transition).
- **integrator / governance**: `docs/STATUS.md` item 13 is updated
  to v0.1; the change-log entry lands; the rev bumped to 5. The v0
  → v1 transition items are: region builder, region safety checks,
  partial rebuild, atomic activation, the guard-failure integration.

## Requested Action

No immediate code change is required from any team. Read the files
that affect your team and reply with corrections:

- **passes**: read `include/b2/pipeline/DirtyClosure.h` and
  `compiler/pipeline/src/DirtyClosure.cpp` and verify the
  `semanticsDependOn` predicate's rules are right. The conservative
  default is: side-effecting users always propagate; pure value
  nodes propagate only through Data/Mem/FrameState/Parent roles;
  Ctrl and None do not propagate. Is this the right cut? Are there
  NodeClass / InputRole combinations where the conservative default
  is wrong (e.g., a pure value node that should NOT propagate dirty
  from a Data input because the value is provably unused)?
- **codegen**: read Section 9 of `docs/partial_deopt_contract.md`
  and verify the guard-failure integration plan is right. The v0.1
  does NOT modify the engine; the v0 → v1 transition adds the call
  to `onGuardFailure()`. Is the integration point (between the
  guard-failure detection and the T0 fallback, lines 900-934 of
  `executeCompiled`) the right place?
- **ir**: read the v0.1's use of the IR's public API
  (`Graph::usesOf()`, `NodeInfo` registry) and verify the
  `roleOfSlot()` helper's slot-to-role mapping is right (it consults
  `info.numFixed`, `info.roles[]`, `info.variadic`, and
  `info.hasFrameState`).
- **all**: read the 12 unit tests in `tests/pipeline/
  DirtyClosureTests.cpp` and verify the test cases are
  representative. The v0 → v1 transition will add more tests as
  the region builder + partial rebuild land.

## Boundaries

The integrator will not modify any team's `compiler/`, `include/`, or
`tests/` write list beyond:

- the new `DirtyClosure.h` header (passes-team area, applied by the
  integrator wearing the passes-team hat per the
  `MSG-20260830-001` precedent);
- the new `DirtyClosure.cpp` implementation (passes-team area);
- the new `tests/pipeline/` directory (TestHarness.h, TestMain.cpp,
  DirtyClosureTests.cpp, CMakeLists.txt — passes-team area, same
  hat);
- the `compiler/pipeline/CMakeLists.txt` (passes-team area) to
  switch `b2_pipeline` from INTERFACE to STATIC and add
  `src/DirtyClosure.cpp` to the sources;
- the top-level `CMakeLists.txt` to add `add_subdirectory(tests/
  pipeline)` to the `B2_BUILD_TESTS` block;
- `docs/partial_deopt_contract.md` Section 5 (integrator-authored;
  the cross-team contract);
- `docs/STATUS.md` (integrator-owned living document);
- `messages/open/MSG-20260918-008-...INFO.md` (this message).

The integrator will not modify `docs/laws.md`,
`docs/deopt_backend.md`, `docs/stencils.md`,
`docs/cpp26_standards.md`, `docs/teams/*`,
`compiler/passes/src/*` (passes team's existing implementations),
`compiler/interp/src/*` (interpreter team), or
`compiler/codegen/src/*` (the T2 lowering and the engine's trap
handler — the v0.1 is shadow-only, no engine changes).

The new `DirtyClosure.h` header and `DirtyClosure.cpp` implementation
live at the paths the ownership map assigns to the passes team
(`compiler/pipeline/` is listed at `passes.write`); the integrator
is landing them because the passes team has not yet written them and
the v0 → v1 transition needs a stable algorithmic surface to target.
Once landed, the passes team owns the algorithm; the integrator will
not modify it further without team approval.

## Response

```text
status:
responder:
date:
notes:
```
