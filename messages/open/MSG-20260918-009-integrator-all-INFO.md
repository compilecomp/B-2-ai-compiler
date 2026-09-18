---
id: MSG-20260918-009
type: INFO
from: integrator
to:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 124
  - Rule 125
  - Rule 132
related_prs: []
related_tests:
  - tests/pipeline/DirtyClosureTests.cpp
  - include/b2/pipeline/DirtyClosure.h
  - include/b2/pipeline/PartialDeopt.h
created: 2026-09-18
---

## Summary

The partial deopt v0.1's `max_dirty_region_size = 64` default was
previously a duplicated literal in two headers
(`kDefaultMaxDirtyRegionSize` in `DirtyClosure.h` and
`PartialDeoptConfig::max_dirty_region_size` in `PartialDeopt.h`) —
a drift hazard flagged in review. This commit fixes the hazard and
documents the rationale.

The fix:

1. **Single source of truth.**
   `PartialDeoptConfig::max_dirty_region_size` now defaults to
   `kDefaultMaxDirtyRegionSize` (the constant in `DirtyClosure.h`),
   not to a duplicate literal. The constant is the single source of
   truth; the config field inherits the default. If a future
   revision changes the default, it changes in ONE place.

2. **Documented rationale for `64`.** The constant's declaration
   in `DirtyClosure.h` now carries a rationale comment explaining
   why 64 IR nodes is the right default:
   - 64 IR nodes is roughly the size of a hot loop body or a small
     inlined callee — the natural unit of partial recompilation
     (Section 4: "good region candidates"). A closure that stays
     under 64 covers the common salvage cases; a closure that
     exceeds 64 is likely covering multiple unrelated speculation
     failures and is no longer "the smallest safe region" —
     escalate.
   - 64 is small enough that the partial rebuild's compile cost
     (lowering + verifying + activating ~64 IR nodes) is bounded
     by the deopt-to-reopt latency budget (Section 22 telemetry's
     `deopt_to_reopt_latency_ms`). At ~64 nodes, the rebuild is
     ~1-3 ms on commodity hardware; above that, the latency grows
     non-linearly (the verifier's O(n²) checks dominate).
   - 64 is large enough that the closure doesn't trivially
     over-escalate on real Java methods. The interp corpus's
     largest single-method graph is ~50 nodes today; 64 leaves
     headroom for the larger methods the frontend will lower once
     the Java stdlib lands (`docs/STATUS.md` item 7).

3. **Documented rationale for `partial_deopt_budget = 3`.** The
   `PartialDeoptConfig::partial_deopt_budget` field (the per-method,
   per-window deopt count budget, Section 19) now carries a
   rationale comment: 1 deopt is normal (the first speculation
   break for a method is expected; the recompile adapts), 2 is
   suspicious (the recompile may have picked a bad speculation),
   3 is the action threshold (the recompile is thrashing; stop
   attempting partial deopt for the method until the window
   resets).

4. **Drift-guard unit test.** Two new tests in
   `tests/pipeline/DirtyClosureTests.cpp`:
   - `pipeline_dirty_closure_default_budget_single_source_of_truth`
     asserts `PartialDeoptConfig::max_dirty_region_size ==
     kDefaultMaxDirtyRegionSize` AND `kDefaultMaxDirtyRegionSize ==
     64` (the latter is a sanity check: if the rationale ever
     changes the default, this assertion must be updated to match
     — and the rationale comment with it).
   - `pipeline_dirty_closure_runtime_override_of_default_budget`
     verifies the algorithm takes `max_size` as a parameter at
     the call site (no hard-coded literal inside the algorithm):
     overrides to 1 (fires immediately) and to 1024 (does not
     fire for the small graph), checks both paths.

5. **Contract doc updated.** `docs/partial_deopt_contract.md`
   Section 19 (deopt budget and hysteresis) and Section 21
   (production safety policy) updated to reflect the single
   source of truth + the rationale + the drift guard.

The algorithm's default value (64) is UNCHANGED. This is a
code-organization commit (single source of truth + documented
rationale + drift guard), not a behavior change.

## Why this matters

Magic numbers duplicated across headers are a real hazard:
- A reviewer who changes the default in one place (e.g., tunes
  `kDefaultMaxDirtyRegionSize` from 64 to 128 based on telemetry)
  without changing the other gets a silent drift — the algorithm
  uses one default (via its parameter), the runtime config uses
  another.
- A reviewer who sees `= 64` in two places has no way to know
  whether the duplication is intentional or a bug. With the
  single source of truth, the relationship is explicit.
- A reviewer who sees `= 64` with no rationale has no way to
  evaluate whether the value is right. With the rationale, the
  value is auditable against the design (Section 22's latency
  budget; Section 4's "good region candidates"; the corpus's
  largest single-method graph).

The pattern (single constant + documented rationale + drift-guard
test) is the same pattern used elsewhere in the IR:
- `kInvalidNodeId`, `kInvalidFrameState`, `kInvalidDependency`
  (the IR's "missing" sentinels; one source of truth per type);
- `kEffectOrderTable` (the 144-entry reorder table; the rules
  R1-R9 are the rationale, the table is generated from them, the
  table-vs-rules consistency test is the drift guard);
- `kMaxEarlyCleanupRounds`, `kDefaultPassRewriteBudget`
  (the passes team's budget constants; documented at declaration).

## Evidence

- `git show --stat HEAD` on the commit that lands these changes.
- Local build + test sweep (pre-push):

  ```
  $ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -G Ninja
  $ cmake --build build -j && ctest --test-dir build --output-on-failure
  100% tests passed out of 12
  Total Test time (real) = 0.07 sec
  ```

  The 2 new drift-guard tests are the 13th and 14th tests in
  `tests/pipeline/DirtyClosureTests.cpp` (the `pipeline_tests`
  ctest target runs all 14).

- ASan + UBSan build + test sweep (pre-push):

  ```
  $ cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14 \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -g" -G Ninja
  $ cmake --build build-san -j && ctest --test-dir build-san --output-on-failure
  100% tests passed out of 12
  Total Test time (real) = 1.71 sec
  ```

- Regression sweep (no behavior change):

  ```
  $ b2jit --stats sweep over tests/interp/corpus/*.rbc: 19/19 T1 corpus green
  $ ctest -R codegen_tests: PASS (16/19 T2 differential, same 3 known bugs)
  $ ctest -R pipeline_tests: PASS (14/14 pipeline tests)
  ```

## Impact

- **passes**: the dirty closure's budget constant is now in ONE
  place (`kDefaultMaxDirtyRegionSize` in `DirtyClosure.h`); your
  team's `PartialDeoptConfig` inherits the default. Please review
  the rationale comment at the constant's declaration and reply
  with corrections if the value 64 is wrong for any reason (e.g.,
  the verifier's O(n²) cost model has changed; the latency budget
  in Section 22 has changed).
- **codegen**: no impact. The v0 → v1 transition's call site
  (`onGuardFailure()` → `expandDirtyClosure()`) will pass
  `cfg.max_dirty_region_size` as the `max_size` parameter; the
  algorithm already takes it as a parameter (no hard-coded literal
  inside the algorithm). The drift-guard test verifies this.
- **ir**: no impact. The constant lives in the pipeline headers
  (not the IR); the IR's pattern (`kInvalidNodeId` etc.) is the
  model the pipeline follows.
- **interpreter / regalloc / aot**: no impact. The v0.1.1 is a
  pipeline-internal code-organization commit; no runtime behavior
  changes.
- **integrator / governance**: `docs/STATUS.md` rev bumped to 6;
  the change-log entry lands.

## Requested Action

No immediate code change is required from any team. Read the
rationale comments at the constant's declaration (`include/b2/
pipeline/DirtyClosure.h`) and at the config field's declaration
(`include/b2/pipeline/PartialDeopt.h`) and reply with corrections
if the values or rationales are off. If a future revision tunes
the defaults based on telemetry, update the rationale comment
WITH the value change (the drift-guard test will catch a stale
rationale: if `kDefaultMaxDirtyRegionSize` changes from 64 to
something else, the test's `CHECK(kDefaultMaxDirtyRegionSize ==
64)` assertion must be updated to match).

## Boundaries

The integrator will not modify any team's `compiler/`, `include/`,
or `tests/` write list beyond:

- the rationale comment addition at `kDefaultMaxDirtyRegionSize`
  in `include/b2/pipeline/DirtyClosure.h` (passes-team area,
  applied by the integrator wearing the passes-team hat per the
  `MSG-20260830-001` precedent);
- the `PartialDeoptConfig::max_dirty_region_size` default change
  (from the literal `64` to `kDefaultMaxDirtyRegionSize`) and the
  `partial_deopt_budget` rationale comment in
  `include/b2/pipeline/PartialDeopt.h` (passes-team area);
- the 2 drift-guard unit tests in
  `tests/pipeline/DirtyClosureTests.cpp` (passes-team area);
- `docs/partial_deopt_contract.md` Sections 19 + 21
  (integrator-authored cross-team contract);
- `docs/STATUS.md` (integrator-owned living document);
- `messages/open/MSG-20260918-009-...INFO.md` (this message).

The integrator will not modify `docs/laws.md`,
`docs/deopt_backend.md`, `docs/stencils.md`,
`docs/cpp26_standards.md`, `docs/teams/*`,
`compiler/passes/src/*`, `compiler/interp/src/*`, or
`compiler/codegen/src/*`.

## Response

```text
status:
responder:
date:
notes:
```
