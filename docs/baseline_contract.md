# B-2 Baseline Contract (v1) — T1 Stencil Plan Builder, Refusals, Telemetry

Owner: Baseline No-IR Team (`baseline_noir`; see
`docs/teams/baseline-noir-team.md`)

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
Amendment A / Amendment B.1: T1 is a no-IR tier; the charter law for this
team. The codegen team consumes the plan this contract describes; the
instantiation contract lives in docs/codegen_contract.md and conflicts
resolve in its favor for the consume-side.
```

This document pins what "T1 baseline plan" means now that the plan builder
ships. The instantiation stage (codegen team, `docs/codegen_contract.md`
SS3-SS10) is unchanged: a `StencilPlan` is the copy-and-patch RECIPE.
This contract defines what produces the recipe and what it must uphold.

Contents: SS1 the inputs; SS2 the algorithm; SS3 the budget (the kill
switch); SS4 the output plan; SS5 the backedge-poll membership check; SS6
refusals; SS7 determinism and golden dumps; SS8 telemetry; SS9
non-guarantees; SS10 future obligations.

---

## SS1. Inputs

```text
  rbc::Program (verified)
        |
        v
  b2::baseline::compilePlan(program, method_index, set, options)
        |
        v
  PlanResult { ok, plan | reason+detail }
```

- A verified `rbc::Program`. The builder trusts `rbc::verify` (the hard
  gate is upstream); it re-checks only what selection itself needs
  (operand ranges, branch-target membership, patch-value widths, its own
  plan invariants). Any failed re-check refuses the plan.
- A `StencilSet` manifest (`include/b2/baseline/StencilSet.h`,
  `b2::baseline::builtinStencilSetV0()`). The set is a frozen build-time
  artifact: opcode → stencil rows, declared holes, superinstruction
  fusion candidates. Selection is a table lookup against this manifest,
  nothing else.
- A `CompileOptions` (`include/b2/baseline/Compiler.h`): superinstruction
  fusion on/off, `require_backedge_polls` on/off. The defaults are
  production; the off mode is the diagnostic / golden-dump mode.

Public surface: `include/b2/baseline/` (`Compiler.h`, `Plan.h`,
`Stencil.h`, `StencilSet.h`).

---

## SS2. The algorithm

The normative algorithm is pinned in `include/b2/baseline/Compiler.h` and
implemented in `compiler/baseline/src/PlanBuilder.cpp`. Summary:

1. **Hard-gate trust.** If the method did not verify, refuse with
   `UnverifiableMethod`. The corpus tests use verified methods; this path
   is defensive totality.
2. **One linear scan over `code[]` with a peephole window.** At each pc,
   attempt the longest superinstruction fusion in the manifest that tiles
   the next k opcodes; if none, fall back to the single-opcode stencil.
   Complexity is O(N × C) with C = candidates per opcode (single digits).
3. **Second linear pass** assembling: the pc map (instance → rbc pc range),
   stack maps (per safepoint), deopt points (per guard/trap/call),
   exception edges (per handler table entry, translated to native offsets).
4. **Backedge-poll membership check** (Rule 88). See SS5 below.
5. **Final self-audit** (`verifyPlan`). Failure is a builder bug — refuse
   with `InternalInvariant`.

NO IR graph is built. NO dataflow. NO worklist. The one fixpoint that
exists (register liveness for the fusion guard, set v3) is a bounded
backward bitset pass over the ORIGINAL rbc pcs serving ONLY the fusion
guard; it creates no nodes, reorders nothing, and its result is a pure
function of (RBC, set, options) (Rule 124). Effect order in the plan IS
RBC order.

---

## SS3. The budget (the kill switch)

`PlanBudget` (`include/b2/baseline/Compiler.h`) is a struct of plain
saturating ceilings. Hitting ANY ceiling refuses the plan (the method
stays on T0 — Amendment A's "always safe to abandon"). Defaults are
named constants sized for v0 methods; they are budget knobs, not tuning
knobs — T1 has no quality axis, only coverage.

| Field | Default ceiling |
|---|---|
| `max_instances` | 65536 |
| `max_output_bytes` | 1 MiB staged code per method |
| `max_patches` | 262144 |
| `max_deopt_points` | 65536 |
| `max_fusions` | 16384 |

Rule 112 (compilation latency and memory budgets must be defined) is
honored here. The builder never stalls a mutator (Rules 11, 13) — there
is no mutator to stall because v0 is synchronous; the multi-threaded
contract is open work documented in `docs/STATUS.md`.

---

## SS4. The output plan

`StencilPlan` (`include/b2/baseline/Plan.h`):

- `instances` — the per-opcode (or per-superinstruction) tiling: each row
  names a `StencilId`, the rbc pc range it covers, the patch values for
  its declared holes, the deopt id (if any), the exception-edge
  destinations (if any).
- `patch_values` — the plan-computable fills (constants, cp indices,
  local offsets) and the runtime-pending fills (call targets, IC stubs).
- `pc_map` — instance → rbc pc range, for deopt reconstruction.
- `stack_maps` — per-safepoint live-reference bitmap (one bit per
  T0-frame slot, the interpreter's value model).
- `deopt_points` — guards, traps, call sites; each carries the ids the
  deopt stub needs (current rbc pc + T0-compatible frame; SS9 of
  `docs/codegen_contract.md`).
- `exception_edges` — handler table translated to native offsets.
- Header fields: `entry_native_offset`, `code_size`, `fusion_count`,
  `patch_count`, `safepoint_count`.

The plan is a pure function of (verified RBC, StencilSet, options) — Rule
124.

---

## SS5. Backedge-poll membership check (Rule 88)

`compiler/baseline/src/PlanBuilder.cpp::checkBackedgePolls` is the
mechanical check for Rule 88 ("JIT code must include safepoint polls at:
loop backedges; ..."). The check is a pure membership test against the
poll bitset computed in step 3; it does no loop analysis.

The check accepts BOTH RBC conventions for placing a poll on a backedge
(relaxed in `MSG-20260918-001`, was form (a) only in v0):

- **form (a)** — the destination of the backward branch IS a
  `safepoint_poll` (poll-at-loop-head). This is the convention the
  frontend lowering uses: `compiler/frontend/src/LowerStmt.cpp` emits
  `Op::SafepointPoll` immediately after the loop-head label in
  `lowerWhile`, `lowerDoWhile`, and `lowerFor`.
- **form (b)** — the instruction immediately preceding the backward
  branch IS a `safepoint_poll` (poll-before-backedge). This is the
  convention the `sum_loop.rbc`-shaped corpus programs use.

Either form alone satisfies the law; mixed forms in the same method are
accepted. The check is a strict superset of the v0 form — every plan
previously accepted is still accepted; the relaxation only admitted new
programs (`sum_loop.rbc` was the canonical holdout).

Backward switch edges (`tableswitch`/`lookupswitch` targeting an earlier
pc) are checked the same way against the poll bitset.

A program that has NO `safepoint_poll` anywhere and a backward branch is
refused with `MissingBackedgePoll`. This is the case for legacy
hand-written corpus programs that did not yet carry polls (none remain
in `tests/interp/corpus/` after `MSG-20260918-002` / `MSG-20260918-003`
landed).

`CompileOptions::require_backedge_polls` toggles the check off for
diagnostic mode (the corpus test `baseline_backedge_poll_disabled_ok`
pins this).

---

## SS6. Refusals

A refusal is a `PlanResult { ok=false, reason, detail }`. Refuse reasons
are a closed `enum class RefuseReason` (Rule 8: no RTTI):

| Reason | Meaning |
|---|---|
| `Ok` | sentinel for success |
| `UnverifiableMethod` | defensive: method did not pass `rbc::verify` |
| `NoStencilForOp` | no Available stencil for the opcode at this pc (e.g. `invokedynamic`, `multianewarray`, `guard_class`, `deopt_trap`) — method stays on T0 |
| `MissingBackedgePoll` | a backward branch's target or instruction-before-branch is not a `safepoint_poll` (Rule 88; see SS5) |
| `BudgetExceeded` | a `PlanBudget` ceiling was hit |
| `InternalInvariant` | `verifyPlan` rejected the emitted plan — a builder bug, fails the suite loudly |

Every refusal carries a non-empty `detail` string (Rule 47 form: file
[ctx]: pc N: error: ...). The `b2plan` driver prints `refused: <reason>
<detail>` and exits 1.

A refusal NEVER produces a partial plan. The method either gets a
complete, verifyPlan-passing plan or it does not get a plan. This is
Amendment A's "always safe to abandon" (Rule 96 form): T0 runs the
method.

---

## SS7. Determinism and golden dumps

Rule 124 (compilation must be deterministic and replayable) is
mechanically checked by the corpus test `tests/baseline/CorpusTest.cpp`
and the golden-dump tests in `tests/baseline/BaselineTests.cpp`. Same
RBC, same StencilSet, same options → byte-identical plan and dump.

The dump format (`b2plan --no-fusion --check-only` for opcode-stencil-only
plans, `b2plan <file>` for full plans with patch values, stack maps,
deopt points, exception edges) is the golden-test proof. The output is
deterministic across runs; no timestamps, no addresses, no
iteration-order dependence.

---

## SS8. Telemetry

Rule 26 (no silent fallbacks without telemetry) and Rule 119 (tier
transitions must be observable) are honored in the `b2jit` driver's
stats line:

```
attempts=N ok=N planRefused=N instRefused=N deopts(trap=N callExc=N guard=N) t0Fallback=N entries=N helperCalls=N codeBytes=N
```

Every plan refusal, every T0 fallback, every deopt is counted. The T1
kill switch (Rule 96 form) is `CompileOptions::require_backedge_polls`
plus the budget — there is no global "disable T1" runtime switch yet;
that is open work tracked in `docs/STATUS.md`.

---

## SS9. Non-guarantees (v1)

- No IR graph (Amendment A). T1 cannot do any optimization that requires
  dataflow; that lives in T2 (`compiler/passes/`, no execution driver
  yet — see `docs/STATUS.md`).
- No multi-threaded compilation. `b2jit` is synchronous. Rules 11 and 13
  are open work.
- No real GC. The interpreter's bump allocator
  (`compiler/interp/src/Heap.cpp`) handles allocation; T1 stack maps
  assume the interpreter value model and are correct under that model,
  but there is no generational collector or write-barrier code to honor
  (the `docs/gc.md` design is unimplemented).
- Single architecture. x86-64 only; the StencilSet manifest's
  `target_arch = 1` rejects any other. ARM64 / RISC-V backends are open
  work.

---

## SS10. Future obligations

- A "disable T1" runtime kill switch (Rule 96 form). Today the kill
  switch is `require_backedge_polls = false` plus the budget; the
  integrator-acknowledged gap is a runtime flag that disables T1
  end-to-end and runs T0, verified by tests.
- Multi-threaded compilation with safepoint handshake (Rules 11, 13).
  Tracked in `docs/STATUS.md`.
- The NaN boxing representation contract (Part XVIII). T1 is an affected
  party; tagged-value register classes in T1 generic slots must remain
  GC-visible, deopt-reconstructible to exact T0 state, and must not
  change Java FP semantics. The feature is disabled by default; this
  team must approve before enablement, plus T1 round-trip tests must
  pass.
- Real GC integration. When `compiler/gc/` ships, T1 stack maps must
  feed the collector's per-safepoint reference bitmap; the current
  stack map format is designed for this (one bit per T0-frame slot).

---

## Change control

Changes to this contract go through the message system under team key
`baseline_noir` (`docs/teams/messaging.md`). Cross-team consumers (codegen
for instantiation, interpreter for the corpus, gc for stack maps) pin
tests against plan output at their own risk until a contract message
lands.
