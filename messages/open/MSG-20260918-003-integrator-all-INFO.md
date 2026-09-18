---
id: MSG-20260918-003
type: INFO
from: integrator
to:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 88
  - Rule 124
  - Rule 145
related_prs: []
related_tests:
  - tests/interp/corpus/fib_loop.rbc
  - tests/interp/corpus/fib_loop.rbc.expected
  - tests/baseline/CorpusTest.cpp
  - tests/codegen/CorpusTest.cpp
created: 2026-09-18
---

## Summary

Three follow-ups to `MSG-20260918-001` land in this commit, closing the
corpus to 19/19 and reconciling the path ownership map with the missing
team contract docs:

1. **`tests/interp/corpus/fib_loop.rbc` now carries a `safepoint_poll`
   at the loop head** (form (a) of the baseline contract, matching the
   frontend lowering convention). The 19th corpus program now compiles
   to machine code; the corpus sweep is 19/19. Closes
   `messages/closed/MSG-20260918-002-baseline_noir-interpreter-BUG.md`.
2. **`tests/baseline/CorpusTest.cpp` floors updated**: the
   `refused >= 1` floor (lowered from 2 in `MSG-20260918-001`) is now
   removed. A refusal is a regression, not a baseline; the test comment
   explains how to add a future intentionally-refusing fixture
   (`invokedynamic` / `multianewarray` holdouts) with a per-method
   assertion rather than a global floor.
3. **Three missing contract docs added** under their team-owned paths:
   - `docs/baseline_contract.md` — v1 contract for the baseline_noir
     team's stencil plan builder: inputs, the linear-scan algorithm,
     the budget (the kill switch), the output plan, the backedge-poll
     membership check (Rule 88), refusals, determinism and golden
     dumps, telemetry, non-guarantees, future obligations.
   - `docs/regalloc_contract.md` — v0 stub. Acknowledges no
     implementation; describes what the regalloc team WILL ship when it
     ships, what it will consume (the machine-level representation from
     codegen lowering), and what it owes consumers (per-safepoint
     reference locations, spill/reload edits under the MIR contract).
     Documents the dependency: regalloc cannot ship until the T2
     execution driver exists and the MIR contract between codegen and
     regalloc is pinned.
   - `docs/aot_contract.md` — v0 stub. Acknowledges no implementation;
     describes what the AOT team WILL ship when it ships (the T2
     pipeline run offline against closed world assumptions, manifests
     with mechanically-checked proofs per Amendment B.5). Documents
     the dependency: AOT cannot ship until T2 driver, the deopt
     backend, and closure analysis all exist.

   The `docs/teams/ownership.yaml` file referenced all three of these
   contract docs as team write paths; their previous absence made the
   ownership map silently inconsistent with the tree. This commit
   reconciles the map with reality.

4. **`docs/STATUS.md` updated** — the corpus sweep count moved from
   18/19 to 19/19; the "missing contract docs" entry in the path
   ownership honesty section is resolved; a new entry on the
   recommended-next-steps list acknowledges that the regalloc and AOT
   team stubs are landed as forward contracts, not implementations.

## Evidence

- `git show --stat HEAD` on the commit that lands these changes.
- Local build + test sweep (pre-push):

  ```
  $ ctest --test-dir build --output-on-failure
  100% tests passed out of 10
  Total Test time (real) =   0.06 sec
  ```

- `b2jit --stats` over `tests/interp/corpus/*.rbc`: 19/19 programs
  report `ok=1 planRefused=0 t0Fallback=0` with non-zero `codeBytes`.
  The two programs that previously fell back to T0 (`fib_loop.rbc` and
  `sum_loop.rbc`) both now compile to machine code.
- The three contract docs are at the paths the ownership map expects;
  `docs/teams/ownership.yaml`'s `baseline_noir.write`,
  `regalloc.write`, and `aot.write` lists now all reference files that
  exist.

## Impact

- **interpreter**: `fib_loop.rbc` now carries a `safepoint_poll` at the
  loop head (form (a)). T0 execution is unaffected (the poll opcode is
  a no-op in T0 today, only `InterpStats` counters it); the
  `fib_loop.rbc.expected` golden twin still matches byte-identically.
- **baseline_noir**: the corpus sweep is now 19/19. The baseline
  contract (`docs/baseline_contract.md`) is now the normative
  description of what your team has shipped; review it and reply with
  corrections. The backedge-poll membership check section (SS5) names
  both accepted forms and cites `MSG-20260918-001` for the relaxation
  audit trail.
- **codegen**: no impact. The T1 instantiation path is unchanged; the
  corpus sweep's 19/19 includes both `fib_loop.rbc` and `sum_loop.rbc`
  as machine code.
- **regalloc / aot**: your team contract docs now exist as v0 stubs.
  They acknowledge no implementation, document the dependencies, and
  are forward contracts. When your team ships v1, send an INFO message
  and update the stub; the integrator will move the v0 stub to a v1
  real contract at that time. No code may land in `compiler/regalloc/`
  or `compiler/aot/` until v1 ships — the paths do not exist; the
  integrator's `docs/STATUS.md` tracks the decision to either add v0
  directory stubs or reconcile `docs/teams/ownership.yaml` to remove
  the paths until v1.
- **frontend**: the lowering already emits `Op::SafepointPoll` at loop
  heads in `lowerWhile`, `lowerDoWhile`, and `lowerFor` — verified by
  lowering `tests/frontend/lower_corpus/control.java` (which contains
  all three loop forms) and confirming the resulting RBC has a
  `safepoint_poll` at every loop head and is T1-plannable end-to-end.
  No frontend change was needed for this commit; the lowering
  convention is already consistent with the new `fib_loop.rbc` form.
- **integrator / governance**: the path ownership map is now
  consistent with the tree. Every team's `write:` list points to paths
  that exist.

## Requested Action

No immediate code change is required from any team. Read your team's
contract doc and reply with corrections if any statement about your
team's path is inaccurate. The integrator will roll corrections into
the next contract revision.

For the regalloc and AOT teams specifically: the v0 stubs describe what
your team WILL ship, not what your team has shipped. If the forward
contract described in the stub is wrong (e.g. the charter's deliverables
have changed since the charter was written, or the dependency
assessment is off), reply with the correction and the stub will be
updated. The stub is the integrator's best reading of your charter
(`docs/teams/regalloc-team.md` and `docs/teams/aot-team.md`); your team
is the authority on whether the reading is right.

## Boundaries

The integrator will not modify any team's `compiler/`, `include/`, or
`tests/` write list beyond:

- the one-line fix to `tests/interp/corpus/fib_loop.rbc` (interpreter
  team's area, applied by the integrator wearing the interpreter-team
  hat per the `MSG-20260830-001` precedent);
- the test comment + floor removal in `tests/baseline/CorpusTest.cpp`
  (baseline_noir team's area, applied by the integrator wearing the
  baseline_noir-team hat).

The integrator will not modify `docs/laws.md`, `docs/deopt_backend.md`,
`docs/stencils.md`, `docs/cpp26_standards.md`, `docs/teams/*`, or any
team's `compiler/`, `include/`, or `tests/` subdirectory beyond the two
edits above.

The three contract docs (`docs/baseline_contract.md`,
`docs/regalloc_contract.md`, `docs/aot_contract.md`) live at the paths
the ownership map assigns to their respective teams; the integrator is
landing them because the teams have not yet written them and the
ownership map's references were dangling. Once landed, the team owns
the doc; the integrator will not modify them further without team
approval.

## Response

```text
status:
responder:
date:
notes:
```
