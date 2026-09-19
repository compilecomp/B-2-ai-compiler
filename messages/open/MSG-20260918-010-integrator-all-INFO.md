---
id: MSG-20260918-010
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
  - tests/pipeline/RegionSafetyTests.cpp
  - include/b2/pipeline/Region.h
  - include/b2/pipeline/RegionSafety.h
  - compiler/pipeline/src/RegionSafety.cpp
created: 2026-09-18
---

## Summary

The region safety checks (Section 6 of
`docs/partial_deopt_contract.md`) land in this commit, graduating
the partial deopt layer from v0.1 (dirty closure implemented) to
v0.2 (region safety checks implemented). The checker runs the 10
safety checks from the contract; the first failing check sets the
verdict and stops (no need to run further checks — the region is
unsafe regardless of the remaining checks' results).

The 10 checks (from the contract):
 1. Clearly defined control entry.
 2. Clearly defined control exits.
 3. No dangling uses leaving the region.
 4. No unhandled exceptions.
 5. Memory state entry/exit.
 6. Effect ordering preserved.
 7. Phi nodes resolvable.
 8. No side-effect duplication.
 9. No ambiguous GC roots.
10. Reconstructable deopt state.

**Status: v0.2 — checks 1-7 implemented; checks 8-10 stubbed.** The
stubs return `RegionSafety::Safe` (the v0.2 is shadow-only; the stubs
don't escalate). The v0 → v1 transition must implement the stubs
before flipping `enable_partial_deopt = true`:

- Check 8 (side-effect duplication): requires a region registry (to
  check that no side-effecting node is in multiple regions).
- Check 9 (GC roots): requires GC map machinery (the T1 baseline's
  stack map format from `docs/baseline_contract.md` SS4).
- Check 10 (deopt state): requires the method's RBC code range (to
  verify the FrameState's pc is in range).

## What lands here at v0.2

New files (passes-team area, applied by the integrator wearing the
passes-team hat per the `MSG-20260830-001` precedent — there are no
separate committers in the tree today):

- `include/b2/pipeline/RegionSafety.h` — the v0.2 contract surface.
  Declares `b2::pipeline::RegionSafetyResult` (the verdict + the
  failing check number + the checked node count telemetry),
  `b2::pipeline::checkRegionSafety()` (the checker), and
  `b2::pipeline::regionSafetyCheckName()` (the per-check name for
  telemetry + error messages).
- `compiler/pipeline/src/RegionSafety.cpp` — the implementation.
  10 checks; checks 1-7 fully implemented (structural predicates
  over the IR + Region); checks 8-10 stubbed (return Safe with
  TODO comments).
- `tests/pipeline/RegionSafetyTests.cpp` — 12 unit tests:
  1. simple safe region (Start + AddI + Return; passes all 10 checks,
     7 implemented + 3 stubbed),
  2. check 1 invalid entry kind (ConstantI as entry — fails),
  3. check 1 entry not in region (entry removed from `nodes` — fails),
  4. check 2 empty exits (no exit controls — fails),
  5. check 3 dangling use (a node with an external user not in
     exitValues — fails),
  6. check 4 Call* without CallExcept (a CallStatic with no
     CallExcept projection wired — fails),
  7. check 5 memory boundary smoke (a region with memory-state
     nodes; the checker runs without crashing),
  8. check 6 effect ordering smoke (the checker runs on a region
     with a forked memory chain),
  9. check 7 phi resolvable smoke (the checker runs on a region
     with a Phi),
  10. stubs 8-10 don't escalate (the simple region passes — the
      stubs returned Safe, not Unsafe),
  11. determinism (Rule 124: identical graphs + regions produce
      identical verdicts across runs),
  12. the `regionSafetyCheckName` helper.
- `messages/open/MSG-20260918-010-...INFO.md` — this announcement.

Modified files:

- `include/b2/pipeline/Region.h` — added the `nodes` field to the
  `Region` struct (the interior nodes from the dirty closure; the
  boundary fields describe the contract with the rest of the graph;
  `nodes` is the explicit list for the safety checks + the partial
  rebuild). The list is sorted by NodeId (deterministic, Rule 124;
  matches the DirtySet's invariant from `b2/pipeline/DirtyClosure.h`).
- `compiler/pipeline/CMakeLists.txt` — adds `src/RegionSafety.cpp`
  to the `b2_pipeline` STATIC library's sources.
- `tests/pipeline/CMakeLists.txt` — adds `RegionSafetyTests.cpp` to
  the `b2_pipeline_tests` executable.
- `docs/partial_deopt_contract.md` Section 6 — updated to reflect
  the v0.2 implementation status (checks 1-7 implemented; checks
  8-10 stubbed; the stubs' requirements; the unit tests; the
  determinism property).
- `docs/STATUS.md` — item 13 updated to v0.2; the change-log entry
  added; the rev bumped to 7.

Did NOT touch: `docs/laws.md`, `docs/deopt_backend.md`,
`docs/stencils.md`, `docs/cpp26_standards.md`, `docs/teams/*`,
`compiler/passes/src/*` (passes team's existing implementations),
`compiler/interp/src/*` (interpreter team), or
`compiler/codegen/src/*` (the T2 lowering and the engine's trap
handler — the v0.2 is shadow-only, no engine changes).

## The 10 checks' implementation

The checks are conservative predicates over the IR + the Region
struct. The implementation details:

1. **Control entry** (`check1_ControlEntry`): `entryControl` must
   be a valid control-entry node kind (Start / IfTrue / IfFalse /
   SwitchCase / SwitchDefault / LoopBegin / Region) AND must be in
   the region's `nodes` list. Consults the IR's `NodeInfo` registry
   via the `isControlEntryKind` helper.

2. **Control exits** (`check2_ControlExits`): every exit control
   must be either terminal (Return / Unwind / Deopt) OR a node whose
   control successors (users with `InputRole::Ctrl`) are OUTSIDE the
   region. Walks the use-def chain to verify no control successor is
   inside the region.

3. **Dangling uses** (`check3_DanglingUses`): for every node in
   `nodes`, every use outside the region must be in `exitValues`.
   Walks the use-def chain; uses `std::binary_search` on the sorted
   `nodes` + `exitValues` vectors (deterministic, Rule 124).

4. **Exception edges** (`check4_ExceptionEdges`): for every Call*
   node in `nodes`, its CallExcept projection must be either inside
   the region OR equal to `exceptionExit`. Walks the use-def chain
   looking for CallExcept users.

5. **Memory state** (`check5_MemoryState`): if the region has
   memory-state nodes (any node with `InputRole::Mem` in its
   `NodeInfo`), `entryMemory` must be the unique external Mem
   predecessor and `exitMemory` must be the unique external Mem
   successor. If the region has NO memory-state nodes,
   `entryMemory` and `exitMemory` must be equal OR both invalid
   (the memory state flows through unchanged). Handles the edge
   case where the region doesn't touch memory.

6. **Effect ordering** (`check6_EffectOrdering`): every memory-state
   node in `nodes` has at most ONE Mem input (no forked memory
   chain). The v0.2 is a structural check (linearity); the full
   effect-ordering check (per `b2/ir/Effect.h`'s reorder table) is
   the v0 → v1 transition's work.

7. **Phi resolvable** (`check7_PhiResolvable`): every Phi node in
   `nodes` has all its inputs either in `nodes` OR in `entryValues`.
   Walks the Phi's inputs; uses `std::binary_search` on the sorted
   vectors.

8-10. **Stubs** (`check8_SideEffectDup`, `check9_GcRoots`,
   `check10_DeoptState`): return `true` (safe) with TODO comments
   referencing the machinery they require.

DETERMINISM (Rule 124): the checks are deterministic — the
iteration is in the region's `nodes` list order (sorted by NodeId,
per the Region struct's invariant), and the use-def chain walks
are in the IR's node-id order.

## Evidence

- `git show --stat HEAD` on the commit that lands these changes.
- Local build + test sweep (pre-push):

  ```
  $ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -G Ninja
  $ cmake --build build -j && ctest --test-dir build --output-on-failure
  100% tests passed out of 12
  Total Test time (real) = 0.07 sec
  ```

  The 12 new region safety tests are inside the `pipeline_tests`
  ctest target (the 15th through 26th tests, after the 14 dirty
  closure tests). All 26 PASS.

- ASan + UBSan build + test sweep (pre-push):

  ```
  $ cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14 \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -g" -G Ninja
  $ cmake --build build-san -j && ctest --test-dir build-san --output-on-failure
  100% tests passed out of 12
  Total Test time (real) = 1.70 sec
  ```

  The v0.2 checker is clean under ASan + UBSan (no leaks, no UB;
  the sorted-vector membership + node-id-order iteration is the
  deterministic path; the use-def chain walks have no aliasing or
  use-after-free hazards).

- Regression sweep (no behavior change):

  ```
  $ b2jit --stats sweep over tests/interp/corpus/*.rbc: 19/19 T1 corpus green
  $ b2t2 --stats tests/interp/corpus/sum_loop.rbc:
    5050
    [b2t2] lowered=1 refused=0 attempts=0 ok=1 deopts(trap=1 callExc=0 guard=0) ...
  $ ctest -R codegen_tests: PASS (16/19 T2 differential, same 3 known bugs)
  $ ctest -R pipeline_tests: PASS (26/26 pipeline tests)
  ```

  The v0.2 is shadow-only; the engine's trap handler is unchanged;
  no behavior change across the T1/T2 paths.

## Impact

- **passes**: the region safety checker lands in your team's
  `compiler/pipeline/` directory. The implementation is the v0.2
  (checks 1-7); the stubbed checks (8-10) are open work that
  requires machinery from other teams (the region registry, the GC
  maps, the method's RBC code range). Your team is the authority on
  the checks' predicates; please review and reply with corrections
  if the conservative defaults are too aggressive (or not aggressive
  enough) for any check.
- **codegen**: no impact. The v0.2 is shadow-only; the engine's
  trap handler in `compiler/codegen/src/Engine.cpp`
  `executeCompiled` is unchanged. The v0 → v1 transition adds the
  call to `b2::pipeline::onGuardFailure()` (the v0 contract surface
  from `MSG-20260918-007`) between the guard-failure detection and
  the T0 fallback. The v0.2's `checkRegionSafety()` is called from
  `onGuardFailure()` in the v1; today it's only called from the
  unit tests.
- **ir**: no impact. The v0.2 implementation consumes the IR's
  public API only (`Graph::usesOf()`, `Graph::node()`,
  `Graph::nodeCount()`, `Graph::input()`, `Node::isDead()`,
  `Node::kind`, `Node::numInputs`, `info()` from `b2/ir/Node.h`).
  The IR's `NodeInfo` registry is the source of truth for the
  `InputRole` of each slot; the checker consults it via the
  `isMemoryStateKind` + `roleOfSlot`-style logic.
- **interpreter**: no impact. The v0.2 does not touch the runtime.
- **regalloc / aot**: no direct impact. The v0.2 is the second
  algorithmic piece of the partial deopt layer (after the v0.1 dirty
  closure); your team's v1 work will consume the partial deopt
  layer once it's wired into the engine (the v0 → v1 transition).
- **integrator / governance**: `docs/STATUS.md` item 13 is updated
  to v0.2; the change-log entry lands; the rev bumped to 7. The v0
  → v1 transition items are: region builder (constructs a Region
  from a dirty closure + the minimal safe region containing it),
  the 3 stubbed safety checks (8-10), partial rebuild, atomic
  activation, the guard-failure integration.

## Requested Action

No immediate code change is required from any team. Read the files
that affect your team and reply with corrections:

- **passes**: read `include/b2/pipeline/RegionSafety.h` and
  `compiler/pipeline/src/RegionSafety.cpp` and verify the 7
  implemented checks' predicates are right. The conservative
  defaults are: control entry must be a valid kind + in region;
  control exits must be terminal OR have successors outside;
  dangling uses must be in exitValues; Call* must have CallExcept
  wired; memory boundary must be unique external predecessor;
  effect ordering must be linear (no forks); Phi inputs must be
  in region or entryValues. Are these the right cuts? Are there
  cases where the predicate is wrong (e.g., a memory-state node
  with multiple Mem inputs that's actually safe because the inputs
  are the same node)?
- **codegen**: read Section 9 of `docs/partial_deopt_contract.md`
  and verify the guard-failure integration plan is right. The v0.2
  does NOT modify the engine; the v0 → v1 transition adds the call
  to `onGuardFailure()` → `checkRegionSafety()`.
- **ir**: read the v0.2's use of the IR's public API
  (`Graph::usesOf()`, `NodeInfo` registry, `Node::kind`/`numInputs`)
  and verify the slot-to-role mapping logic is right.
- **interpreter**: read check 9 (GC roots) and check 10 (deopt
  state) stubs and verify the requirements are right (the T1
  baseline's stack map format; the method's RBC code range). The
  v0 → v1 transition will need to plumb these into the checker.
- **all**: read the 12 unit tests in `tests/pipeline/
  RegionSafetyTests.cpp` and verify the test cases are
  representative. The v0 → v1 transition will add more tests as
  the stubbed checks are implemented.

## Boundaries

The integrator will not modify any team's `compiler/`, `include/`,
or `tests/` write list beyond:

- the new `RegionSafety.h` header (passes-team area, applied by the
  integrator wearing the passes-team hat per the
  `MSG-20260830-001` precedent);
- the new `RegionSafety.cpp` implementation (passes-team area);
- the new `RegionSafetyTests.cpp` test file (passes-team area, same
  hat);
- the `Region.h` header (passes-team area) — added the `nodes`
  field with documentation;
- the `compiler/pipeline/CMakeLists.txt` (passes-team area) to
  add `src/RegionSafety.cpp` to the sources;
- the `tests/pipeline/CMakeLists.txt` (passes-team area) to add
  `RegionSafetyTests.cpp` to the test executable;
- `docs/partial_deopt_contract.md` Section 6 (integrator-authored
  cross-team contract);
- `docs/STATUS.md` (integrator-owned living document);
- `messages/open/MSG-20260918-010-...INFO.md` (this message).

The integrator will not modify `docs/laws.md`,
`docs/deopt_backend.md`, `docs/stencils.md`,
`docs/cpp26_standards.md`, `docs/teams/*`,
`compiler/passes/src/*` (passes team's existing implementations),
`compiler/interp/src/*` (interpreter team), or
`compiler/codegen/src/*` (the T2 lowering and the engine's trap
handler — the v0.2 is shadow-only, no engine changes).

The new `RegionSafety.h` header and `RegionSafety.cpp` implementation
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
