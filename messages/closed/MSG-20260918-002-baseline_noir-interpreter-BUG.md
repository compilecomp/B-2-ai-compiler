---
id: MSG-20260918-002
type: BUG
from: baseline_noir
to:
  - interpreter
severity: P3
status: CLOSED
laws_refs:
  - Rule 88
  - Rule 96
related_prs: []
related_tests:
  - tests/interp/corpus/fib_loop.rbc
  - tests/interp/corpus/fib_loop.rbc.expected
  - tests/baseline/CorpusTest.cpp
  - tests/codegen/CorpusTest.cpp
created: 2026-09-18
closed: 2026-09-18
---

## Summary

`tests/interp/corpus/fib_loop.rbc` has no `safepoint_poll` instruction
anywhere in its body. Under Rule 88 ("Generated Code Must Poll Safepoints
— JIT code must include safepoint polls at: loop backedges; ..."), the
baseline T1 plan builder refuses to compile this method and the run
silently falls back to T0. This is the only corpus program refused under
the poll reason after the baseline's contract relaxation in
MSG-20260918-001.

## Evidence

Method body (RBC text):

```
.method static main ()V
.regs 6
.locals 3
.const c0 = field java/lang/System out Ljava/io/PrintStream;
.const c1 = method java/io/PrintStream println (I)V
iconst r0 0
istore r0 l0
iconst r0 1
istore r0 l1
iconst r0 15
istore r0 l2
Lloop:
iload r0 l2
ifle r0 Ldone
iload r0 l0
iload r1 l1
iadd r2 r0 r1
iload r0 l1
istore r0 l0
istore r2 l1
iload r3 l2
iinc r3 -1
istore r3 l2
goto Lloop
Ldone:
getstatic r0 c0
iload r1 l0
invokevirtual r2 r0 r2 c1
return
.end
```

There is no `safepoint_poll` opcode (`Op::SafepointPoll`, see
`include/b2/rbc/Opcode.h:32`) between `Lloop:` and `goto Lloop`.

Replay artifact:

```
$ ./build/compiler/baseline/b2plan tests/interp/corpus/fib_loop.rbc
refused: missing-backedge-poll backward branch at pc 14 targets pc 6 which is not a safepoint_poll

$ ./build/compiler/codegen/b2jit tests/interp/corpus/fib_loop.rbc --stats
610
[b2jit] attempts=1 ok=0 planRefused=1 instRefused=0 deopts(trap=0 callExc=0 guard=0) t0Fallback=1 entries=0 helperCalls=0 codeBytes=0
```

The `t0Fallback=1` is the safe T0 fallback (Amendment A: always safe to
abandon; Rule 96: T0 runs the method). The run produces correct output
(`610`), but the corpus differential law's intent — every corpus program
exercises the T1 machine-code path — is undermined. With this fixed, the
corpus sweep would be 19/19 instead of 18/19.

## Impact

- **baseline_noir**: cannot compile this method under the current
  contract; falls back to T0. Correctness is preserved (Rule 96); only
  the corpus differential law's "compiled >= 5" floor is weaker than it
  should be.
- **codegen**: same — the `b2jit` execution path falls back to T0 for
  this corpus program.
- **interpreter**: T0 execution of `fib_loop.rbc` is unaffected — the
  poll opcode is a no-op in T0 today (only InterpStats counter). Adding
  the poll should not change `fib_loop.rbc.expected`.

## Requested Action

Add a `safepoint_poll` instruction to `tests/interp/corpus/fib_loop.rbc`
in one of the two accepted positions (see MSG-20260918-001 for the
contract relaxation):

- preferred: at the loop head, immediately after `Lloop:` (form (a),
  matches the existing `sum_loop.rbc` convention's intent and the
  interpreter team's "poll on the backedge" comment in the file header
  of `sum_loop.rbc`), OR
- alternatively: immediately before the `goto Lloop` (form (b), matches
  the actual placement in `sum_loop.rbc`).

Either form is now accepted by the baseline; pick whichever your team
prefers and apply consistently. If the interpreter team has a separate
convention (e.g. polls are auto-inserted by the quickener, not the
author), reply with that convention and we will update the corpus comment
to match.

## Boundaries

The baseline_noir team did not modify `compiler/interp/`,
`tests/interp/`, or `docs/interp_contract.md`. The fix to
`fib_loop.rbc` is the interpreter team's call.

## Resolution

Resolved in the same commit by the integrator (B-2 Architect, wearing
the interpreter-team hat per the ME-INT precedent — see
`messages/closed/MSG-20260830-001-interpreter-ir-BUG.md` for the prior
precedent): a `safepoint_poll` instruction was placed immediately after
the `Lloop:` label in `tests/interp/corpus/fib_loop.rbc` (form (a) of
the baseline contract — poll-at-loop-head). This matches the frontend
lowering convention in `compiler/frontend/src/LowerStmt.cpp`
(`lowerWhile`, `lowerDoWhile`, `lowerFor` all emit
`Op::SafepointPoll` immediately after the loop-head label), so
hand-written corpus programs and `b2parse --emit-rbc` output are
T1-plannable under the same rule.

Verified end-to-end after the fix:

- `b2run tests/interp/corpus/fib_loop.rbc` produces `610` — matches
  `fib_loop.rbc.expected` (T0 execution is unaffected; the poll opcode
  is a no-op in T0 today, only InterpStats counters it).
- `b2plan tests/interp/corpus/fib_loop.rbc` succeeds: 17 instances
  planned, 2 stack maps, 2 deopt points.
- `b2jit tests/interp/corpus/fib_loop.rbc --stats` reports
  `ok=1 planRefused=0 t0Fallback=0 codeBytes=437`.
- `ctest --test-dir build --output-on-failure`: 10/10 ctest targets
  pass.
- Corpus sweep is now 19/19 (was 18/19 after the backedge-poll
  relaxation in `MSG-20260918-001`, was 17/19 before that commit).

The `refused >= 1` floor in `tests/baseline/CorpusTest.cpp` was removed
(its history: floor was 2 in v0, lowered to 1 in
`MSG-20260918-001`, removed in the same commit that lands this fix —
`MSG-20260918-003`). A refusal is now a regression, not a baseline; the
test comment explains how to add a future intentionally-refusing
fixture with a per-method assertion rather than a global floor.

## Response

```text
status: CLOSED
responder: B-2 Architect (integrator, wearing the interpreter-team hat)
date: 2026-09-18
notes: safepoint_poll placed at the loop head (form (a)) to match the
       frontend lowering convention; corpus 19/19; all ctest targets green.
       The poll-before-backedge form (b) was also acceptable per
       MSG-20260918-001; form (a) was chosen for consistency with
       b2parse --emit-rbc output.
```
