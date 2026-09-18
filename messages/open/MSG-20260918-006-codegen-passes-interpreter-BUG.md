---
id: MSG-20260918-006
type: BUG
from: codegen
to:
  - codegen
  - passes
  - interpreter
  - integrator
severity: P2
status: OPEN
laws_refs:
  - Rule 36
  - Rule 40
  - Rule 96
  - Rule 124
related_prs: []
related_tests:
  - tests/interp/corpus/conversions.rbc
  - tests/interp/corpus/conversions.rbc.expected
  - tests/interp/corpus/fields.rbc
  - tests/interp/corpus/fields.rbc.expected
  - tests/interp/corpus/float_math.rbc
  - tests/interp/corpus/float_math.rbc.expected
  - tests/interp/corpus/strings_intern.rbc
  - tests/interp/corpus/strings_intern.rbc.expected
  - tests/codegen/T2CorpusTest.cpp
created: 2026-09-18
---

## Summary

The T2 execution driver (landed in `MSG-20260918-005`,
`compiler/codegen/tools/b2t2.cpp`) wires the RBC -> IR -> machine-code
-> execute path end-to-end. The differential contract (Rule 36 form:
T2 byte-identical to T0 on the interpreter corpus) holds for 16 of the
19 corpus programs in the default (no-opt) configuration. This message
tracks the 4 known divergences:

| # | Program | Mode | Symptom |
|---|---|---|---|
| 1 | `conversions.rbc` | no-opt | T2 prints an extra `44` line (T0: `44\n65535\n-1\n2147483647\n-2147483648\n`; T2: `44\n44\n65535\n-1\n2147483647\n`). Likely a T2 lowering bug in the `i2b`/`i2c`/`i2s` + width-widening path that emits a stale value into the wrong slot. |
| 2 | `fields.rbc` | no-opt | T2 throws `java.lang.InternalError: quickened getfield of unwritten field (v0)`. T0/T1 avoid this by lazy-initializing the field slot on first access; T2's `getfield` helper path hits the runtime's quickened-getfield-of-unwritten-field InternalError instead. |
| 3 | `float_math.rbc` | no-opt | T2 prints a leading empty line (T0: `3.75\n0.3333333333333333\n0.1\n`; T2: `\n3.75\n0.3333333333333333\n0.1\n`). Likely a T2 lowering bug in the `float`/`double` constant or `println(F)V`/`println(D)V` arg-staging path that emits an extra `println()` call with no arg. |
| 4 | `strings_intern.rbc` | opt-only | When `b2t2 -O` runs the optimization pipeline (`runInlining` + `runEarlyCleanup`), T2 produces NO output (T0: `1\n0\n`). The SCCP-folded graph (the `Phi` becomes `ConstantI 1` because the `RefEq` of two equal `ConstantSym`s is folded to true) is not correctly handled by the T2 lowering: the post-opt `CallVirtual` arg is a `ConstantI` directly, and the lowering either skips the `println` call or stages the args wrong. |

Items 1-3 are T2 lowering bugs in the no-opt configuration. Item 4 is
a passes+codegen contract bug in the opt-mode path. They are tracked
in one message because the fix order may overlap (fixing item 4 first
may surface the items 1-3 root cause, or vice versa).

The T2 differential test (`tests/codegen/T2CorpusTest.cpp`) SKIPS
items 1-3 (in `kKnownBugs`) so the rest of the corpus remains a
meaningful regression gate. Item 4 is not in the test (the test runs
no-opt only); the `--opt` mode is documented as known-broken in
`docs/t2_driver_contract.md`.

## Evidence

- `b2t2 --stats tests/interp/corpus/conversions.rbc` — T2 output
  diverges at the 2nd line (extra `44`).
- `b2t2 tests/interp/corpus/fields.rbc` — T2 throws
  `Exception in thread "main" java.lang.InternalError: quickened
  getfield of unwritten field (v0)`; T0/T1 print `3`.
- `b2t2 tests/interp/corpus/float_math.rbc` — T2 output starts with
  a leading `\n` (extra empty `println()`).
- `b2t2 -O tests/interp/corpus/strings_intern.rbc` — T2 produces no
  output; `b2t2 tests/interp/corpus/strings_intern.rbc` (no opt)
  produces `1\n0\n` correctly.
- `tests/codegen/T2CorpusTest.cpp` `kKnownBugs` set lists items 1-3
  with this message id; the test PASSes today because of the skip.

The post-opt IR dump (captured via `b2t2debug --opt
tests/interp/corpus/strings_intern.rbc`, the temporary debug tool
landed alongside `b2t2`) shows the SCCP pass folded:

```
n4 RefEq n2 n3        [dead@3]   # folded to ConstantI 1 (n39)
n11 Phi n10 n8 n9     [dead@4]   # folded to ConstantI 1 (n41)
; replaced n4 -> n39 (epoch 3)
; replaced n11 -> n41 (epoch 4)
; replaced n39 -> n9 (epoch 8)   # dedup with existing ConstantI 1
; replaced n41 -> n9 (epoch 9)  # dedup
```

After the fold, the first `CallVirtual` (n18) has `n9 (ConstantI 1)` as
its arg (input(3) instead of n11). The T2 lowering's `CallVirtual`
emission should stage `n9`'s slot value into the arg slot; either the
slot for `n9` is not materialized before the call site, or the
lowering's arg-staging loop reads from the wrong slot index.

The likely root cause for item 4 is that the lowering's per-block
emission order does not guarantee that a `ConstantI` referenced by a
`CallVirtual` in block N has been materialized to its slot in a
DOMINATING block. The post-opt IR has `n9` in block `Leq` (the
post-IfTrue merge), but the `CallVirtual` is in the same block, so
the materialization should run before the call. Either the block
ordering is wrong, or the materialization is skipped because the
`ConstantI` is referenced as a value (not as a side-effecting node)
and the lowering assumes the slot was populated by a previous block.

## Impact

- **codegen**: the T2 lowering has 4 known divergences from T0. The
  v0 driver is shipped with `--opt` off by default and 3 corpus
  programs skipped in the differential test. The 4 bugs are the v0 →
  v1 transition's blockers for any performance claim (the v0 is wired
  end-to-end and differentially safe on 16/19, but not yet a perf win
  vs T1/T0).
- **passes**: item 4 may be a passes-team bug (the SCCP fold produces
  a graph that the lowering can't handle) OR a codegen-team bug (the
  lowering doesn't handle the post-opt graph correctly). The
  passes-team owns the SCCP pass; the codegen-team owns the lowering.
  Root-cause attribution is part of the fix.
- **interpreter**: item 2 is partially an interpreter-team concern
  (the runtime's `getfield` helper path raises
  `quickened-getfield-of-unwritten-field` InternalError on first
  access to an unwritten field; T0 avoids this by lazy-init in the
  interpreter loop). The runtime team owns the helper's behavior.
- **integrator / governance**: the T2 driver is shipped with
  documented divergences; `docs/STATUS.md` item 4 is partially closed
  (driver wired + 16/19 differential); the remaining work is tracked
  here.

## Requested Action

**codegen**: investigate and fix items 1, 3, and 4 (the lowering-side
bugs). Item 2 may require coordination with the interpreter team (the
runtime's `getfield` helper behavior).

**passes**: investigate item 4 — is the SCCP-folded graph valid? If
yes, the bug is in the codegen lowering; if no, the SCCP pass needs a
soundness fix (the fold of `Phi` to `ConstantI` based on a
non-constant `RefEq` may be unsound if the `RefEq` is not actually
constant-foldable in the SCCP lattice).

**interpreter**: investigate item 2 — is the
`quickened-getfield-of-unwritten-field` InternalError correct
behavior, or should the helper lazy-init the field to `0`/`null` on
first access like T0 does? If the helper's current behavior is
correct, the T2 lowering must avoid calling it for unwritten fields
(e.g., by inserting a `ConstantI 0` slot-store before the `getfield`).

## Boundaries

The codegen team will not modify `compiler/passes/` (passes team's
area) or `compiler/interp/` (interpreter team's area) for items 1-3
(no-opt lowering bugs) without coordination. The codegen team MAY
modify `compiler/codegen/src/T2Lowering.cpp` for items 1, 3, and 4
(codegen team's area).

The passes team will not modify `compiler/codegen/src/T2Lowering.cpp`
(codegen team's area) for item 4 without coordination. The passes team
MAY modify `compiler/passes/src/SCCP.cpp` and related files if the
root cause is an SCCP soundness bug.

The interpreter team will not modify `compiler/codegen/` or
`compiler/passes/` for item 2 without coordination. The interpreter
team MAY modify `compiler/interp/src/Runtime.cpp` (the helper
behavior) if the lazy-init path is the right fix.

The integrator will not modify any team's `compiler/`, `include/`, or
`tests/` subdirectory for this bug without explicit team approval. The
`tests/codegen/T2CorpusTest.cpp` `kKnownBugs` set is integrator-owned
and tracks which programs are skipped; entries are removed when the
corresponding bug is fixed (the test then enforces the program against
the T0 golden twin).

## Response

```text
status:
responder:
date:
notes:
```
