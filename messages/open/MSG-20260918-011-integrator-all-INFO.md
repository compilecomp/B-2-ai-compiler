---
id: MSG-20260918-011
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
  - Rule 42
  - Rule 96
  - Rule 122
  - Rule 124
  - Rule 125
  - Amendment B.1
related_prs: []
related_tests:
  - tests/pipeline/DependencyIndexTests.cpp
  - include/b2/pipeline/DependencyIndex.h
  - compiler/pipeline/src/DependencyIndex.cpp
created: 2026-09-18
---

## Summary

The DependencyIndex (the runtime inverse map) lands in this commit,
graduating the partial deopt layer from v0.2 (region safety checks)
to v0.3 (DependencyIndex implemented). The index is the bridge
between the IR's per-node `SpecMeta.dependency` (the compile-time
scaffolding) and the runtime's dirty-node closure (Section 5) +
region safety checks (Section 6).

The inverse map: `DependencyId → set<(NodeId, RegionId, MethodId)>`.
When an assumption breaks (e.g., a new subclass is loaded that
invalidates a `ClassHierarchy` assumption), the runtime calls
`invalidate(dep)`; the index returns the dirty set (all nodes +
regions that depend on `dep`). The dirty set feeds into
`expandDirtyClosure()` (v0.1, from `MSG-20260918-008`) +
`checkRegionSafety()` (v0.2, from `MSG-20260918-010`).

The v0.3 is the third algorithmic piece of the partial deopt layer.
Combined with the v0.1 dirty closure + the v0.2 region safety
checker, the three pieces form the partial deopt path's algorithmic
core. The v0 → v1 transition wires them into the engine's
guard-failure handler (Section 9).

## Speculative devirtualization survey

This commit also includes the speculative-devirtualization survey
requested by the user. The findings:

**The GuardInline machinery (ICDG Phase 2) is fully implemented +
tested.** The inline pass (`compiler/passes/src/Inline.cpp`) creates:

- A `TypeProfile` Guard node with `InstanceOf(receiver, profiledClass)`
  as the condition (the speculative devirtualization check),
- A `ClassHierarchy` dependency via `Graph::addDependency()` (Rule 42:
  the assumption that no future subclass will change dispatch at this
  site),
- A `SpecMeta{TypeMonomorphic, PGO, confidence, guard, dep}` attached
  to the guard (Rule 122: the complete speculation metadata),
- The callee's body inlined behind the guard (the speculative
  direct call).

20+ `il_guard_*` tests in `tests/passes/InlineTests.cpp` cover the
classification (mono/bi/poly/mega, confidence, agreement), the guard
construction, the deopt wiring, the kill switch, the end-to-end T0
snapshot path, and the corpus sweep.

**The gap is the runtime invalidation path.** The `ClassHierarchy`
dependency is registered via `Graph::addDependency()` (which returns
a `DependencyId`), but nothing in the runtime tracks which Guard
nodes depend on which assumption. The DependencyIndex (the inverse
map) was a v0 contract surface (from `MSG-20260918-007`) with
no-op implementations. This commit implements the real inverse map.

**The v0.3 is NOT yet wired to the inline pass.** The inline pass
creates the `ClassHierarchy` dependency but does NOT call
`DependencyIndex::record()` — the wiring is the v0 → v1 transition's
work. Today the DependencyIndex is exercised by unit tests only
(including the `pipeline_dependency_index_devirt_scenario` test
that models the ICDG Phase 2 path).

## What lands here at v0.3

New files (passes-team area, applied by the integrator wearing the
passes-team hat per the `MSG-20260830-001` precedent — there are no
separate committers in the tree today):

- `compiler/pipeline/src/DependencyIndex.cpp` — the implementation.
  The inverse map is a
  `std::vector<std::pair<DependencyId, DependencyEntry>>` kept
  sorted by `(dep, node, region)` for:
  - deterministic `invalidate()` output (walk in sorted order;
    Rule 124),
  - O(log n) binary search for duplicate detection in `record()`,
  - O(n) `retireMethod()` (walk once, remove matching).
- `tests/pipeline/DependencyIndexTests.cpp` — 12 unit tests:
  1. empty index,
  2. record + invalidate,
  3. multiple associations per dep (one ClassHierarchy dep may be
     referenced by multiple Guard nodes across multiple methods),
  4. dedup (duplicate `record()` calls are deduplicated),
  5. sorted output (`invalidate()` returns a sorted, deduplicated
     dirty set; Rule 124),
  6. unknown dep (`invalidate()` for an unknown dep returns empty),
  7. `retireMethod()` (removes all associations for a method),
  8. `distinctDependencies()` telemetry,
  9. determinism (Rule 124: the same sequence of `record()` calls
     produces the same dirty set across runs, regardless of
     insertion order — the entries_ vector is kept sorted),
  10. the speculative-devirtualization scenario (a ClassHierarchy
      dependency fires when a new subclass is loaded →
      `invalidate()` returns the Guard node + its region; the dirty
      set feeds into `expandDirtyClosure()` + `checkRegionSafety()`),
  11. invalid dep ignored (`record()` with `kInvalidDependency` is
      a no-op),
  12. no-node-no-region ignored (`record()` with both node and
      region invalid is a no-op).
- `messages/open/MSG-20260918-011-...INFO.md` — this announcement.

Modified files:

- `include/b2/pipeline/DependencyIndex.h` — replaced the v0
  contract surface (no-op inline bodies) with the v0.3 real API:
  `record(dep, node, region, method)`, `invalidate(dep) → DirtySet`,
  `retireMethod(method)`, `size()`, `distinctDependencies()`. Added
  the `DependencyEntry` struct (one association: node + region +
  method). The dirty set's invariants are now documented (sorted +
  deduplicated).
- `compiler/pipeline/CMakeLists.txt` — adds `src/DependencyIndex.cpp`
  to the `b2_pipeline` STATIC library's sources.
- `tests/pipeline/CMakeLists.txt` — adds `DependencyIndexTests.cpp`
  to the `b2_pipeline_tests` executable.
- `docs/partial_deopt_contract.md` Section 2 — updated to reflect
  the v0.3 implementation status (the inverse map is a real sorted
  vector; the API; the thread-safety caveat; the "not yet wired to
  the inline pass" note; the unit tests; the devirt scenario test).
- `docs/STATUS.md` — the change-log entry added; the rev bumped to 8.

Did NOT touch: `docs/laws.md`, `docs/deopt_backend.md`,
`docs/stencils.md`, `docs/cpp26_standards.md`, `docs/teams/*`,
`compiler/passes/src/*` (passes team's existing implementations —
the inline pass is unchanged; the wiring to DependencyIndex is the
v0 → v1 transition's work), `compiler/interp/src/*` (interpreter
team), or `compiler/codegen/src/*` (the T2 lowering and the
engine's trap handler — the v0.3 is shadow-only, no engine changes).

## The speculative-devirtualization scenario test

The `pipeline_dependency_index_devirt_scenario` test models the
ICDG Phase 2 path end-to-end (without the engine integration):

```text
// The inline pass created:
//   - Guard node n42 (the TypeProfile guard)
//   - ClassHierarchy dependency dep=7 (target = the profiled class)
//   - Region r3 (the guard's recompilation region)
//   - Method m1 (the method the guard is in)
idx.record(classHierDep, guardNode, guardRegion, method);
// The runtime fires invalidate() when a new subclass is loaded.
const auto dirty = idx.invalidate(classHierDep);
CHECK(dirty.nodes[0] == guardNode);
CHECK(dirty.regions[0] == guardRegion);
// The dirty set feeds into expandDirtyClosure() + checkRegionSafety().
```

This is the mechanical check that the DependencyIndex correctly
bridges the IR's per-node SpecMeta.dependency to the runtime's
dirty-node closure. The v0 → v1 transition wires the actual
inline pass to call `record()` when it creates a GuardInline guard.

## Evidence

- `git show --stat HEAD` on the commit that lands these changes.
- Local build + test sweep (pre-push):

  ```
  $ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -G Ninja
  $ cmake --build build -j && ctest --test-dir build --output-on-failure
  100% tests passed out of 12
  Total Test time (real) = 0.07 sec
  ```

  The 12 new DependencyIndex tests are inside the `pipeline_tests`
  ctest target (the 27th through 38th tests, after the 14 dirty
  closure + 12 region safety tests). All 38 PASS.

- ASan + UBSan build + test sweep (pre-push):

  ```
  $ cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14 \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -g" -G Ninja
  $ cmake --build build-san -j && ctest --test-dir build-san --output-on-failure
  100% tests passed out of 12
  Total Test time (real) = 1.77 sec
  ```

- Regression sweep (no behavior change):

  ```
  $ b2jit --stats sweep over tests/interp/corpus/*.rbc: 19/19 T1 corpus green
  $ ctest -R codegen_tests: PASS (16/19 T2 differential, same 3 known bugs)
  $ ctest -R pipeline_tests: PASS (38/38 pipeline tests)
  ```

  The v0.3 is shadow-only; the engine's trap handler is unchanged;
  no behavior change across the T1/T2 paths.

## Impact

- **passes**: the DependencyIndex lands in your team's
  `compiler/pipeline/` directory. The v0 → v1 transition wires the
  inline pass to call `record()` when it creates a GuardInline guard
  (the inline pass already creates the `ClassHierarchy` dependency
  via `Graph::addDependency()`; the wiring adds the
  `DependencyIndex::record()` call). Your team is the authority on
  the inline pass's GuardInline path; please review the devirt
  scenario test and verify the association shape is right.
- **codegen**: no impact. The v0.3 is shadow-only; the engine's
  trap handler is unchanged. The v0 → v1 transition adds the call
  to `onGuardFailure()` → `invalidate()` → `expandDirtyClosure()`
  → `checkRegionSafety()` between the guard-failure detection and
  the T0 fallback.
- **ir**: no impact. The v0.3 implementation consumes the IR's
  `DependencyId`, `NodeId`, `MethodId` types only.
- **interpreter**: no impact. The v0.3 does not touch the runtime.
  The v0 → v1 transition may add a class-loading hook that fires
  `invalidate()` when a new subclass is loaded (the runtime's
  `classId()` lookup could trigger it).
- **regalloc / aot**: no direct impact. The v0.3 is the third
  algorithmic piece of the partial deopt layer; your team's v1
  work will consume the partial deopt layer once it's wired into
  the engine.
- **integrator / governance**: `docs/STATUS.md` change-log entry
  lands; the rev bumped to 8. The v0 → v1 transition items are:
  wire the inline pass to `DependencyIndex::record()`, the region
  builder, the 3 stubbed safety checks (8-10), partial rebuild,
  atomic activation, the guard-failure integration.

## Requested Action

No immediate code change is required from any team. Read the files
that affect your team and reply with corrections:

- **passes**: read `include/b2/pipeline/DependencyIndex.h` and
  `compiler/pipeline/src/DependencyIndex.cpp` and verify the
  inverse map's API is right for the inline pass's GuardInline
  path. The v0 → v1 transition will add a
  `DependencyIndex::record(dep, guardNode, regionId, methodId)`
  call in `compiler/passes/src/Inline.cpp` at the point where the
  inline pass creates the ClassHierarchy dependency + SpecMeta.
  Is the association shape (dep, node, region, method) right? Is
  the MethodId available at the call site (the inline pass knows
  the caller's method)?
- **codegen**: read Section 9 of `docs/partial_deopt_contract.md`
  and verify the guard-failure integration plan is right.
- **interpreter**: read the devirt scenario test
  (`pipeline_dependency_index_devirt_scenario`) and verify the
  runtime invalidation path is right. The v0 → v1 transition may
  add a class-loading hook in the runtime that fires
  `invalidate()` when a new subclass is loaded.
- **all**: read the speculative-devirtualization survey above and
  verify the gap analysis is right. The GuardInline machinery is
  fully implemented + tested; the gap is the runtime invalidation
  path (now v0.3-implemented) + the wiring to the inline pass.

## Boundaries

The integrator will not modify any team's `compiler/`, `include/`,
or `tests/` write list beyond:

- the new `DependencyIndex.cpp` implementation (passes-team area,
  applied by the integrator wearing the passes-team hat per the
  `MSG-20260830-001` precedent);
- the `DependencyIndex.h` header (passes-team area) — replaced the
  v0 contract surface with the v0.3 real API;
- the new `DependencyIndexTests.cpp` test file (passes-team area,
  same hat);
- the `compiler/pipeline/CMakeLists.txt` (passes-team area) to
  add `src/DependencyIndex.cpp` to the sources;
- the `tests/pipeline/CMakeLists.txt` (passes-team area) to add
  `DependencyIndexTests.cpp` to the test executable;
- `docs/partial_deopt_contract.md` Section 2 (integrator-authored
  cross-team contract);
- `docs/STATUS.md` (integrator-owned living document);
- `messages/open/MSG-20260918-011-...INFO.md` (this message).

The integrator will not modify `docs/laws.md`,
`docs/deopt_backend.md`, `docs/stencils.md`,
`docs/cpp26_standards.md`, `docs/teams/*`,
`compiler/passes/src/*` (passes team's existing implementations —
the inline pass is unchanged; the wiring to DependencyIndex is the
v0 → v1 transition's work), `compiler/interp/src/*` (interpreter
team), or `compiler/codegen/src/*` (the T2 lowering and the
engine's trap handler — the v0.3 is shadow-only, no engine changes).

The new `DependencyIndex.cpp` implementation lives at the path the
ownership map assigns to the passes team (`compiler/pipeline/` is
listed at `passes.write`); the integrator is landing it because the
passes team has not yet written it and the v0 → v1 transition needs
a stable inverse-map surface to target. Once landed, the passes team
owns the algorithm; the integrator will not modify it further without
team approval.

## Response

```text
status:
responder:
date:
notes:
```
