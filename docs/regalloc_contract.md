# B-2 RegAlloc Contract (v0 stub)

Owner: RegAlloc Team (`regalloc`; see `docs/teams/regalloc-team.md`)

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

**Status: v0 stub — no implementation.** `compiler/regalloc/`,
`tests/regalloc/`, and any allocator code do not exist in the tree today.
This document is the team's forward contract: it describes what the
regalloc team WILL ship when it ships, what it will consume, and what it
owes consumers. The integrator's `docs/STATUS.md` carries the open-work
entry; this contract is the design hook for upstream and downstream
teams.

---

## Why this stub exists

`docs/teams/ownership.yaml` lists `compiler/regalloc/`, `tests/regalloc/`,
and `docs/regalloc_contract.md` as the regalloc team's write paths. The
charter (`docs/teams/regalloc-team.md`) describes a full liveness-based
register allocator. The code is not yet written; a real T2 execution
driver (the consumer of the allocator's output) is also not yet written
(see `docs/STATUS.md`). Rather than leave the contract doc referenced
but missing — which makes the ownership map and the charter silently
inconsistent — the integrator is landing this v0 stub now.

When the regalloc team ships, this stub will be replaced with a v1
contract that pins:

- the allocator's inputs (the machine-level representation produced by
  codegen lowering, per the team charter's "liveness analysis over the
  machine-level representation produced by codegen lowering");
- the allocator's outputs: virtual register assignments per register
  class, spill-slot layout and frame-size contribution, spill/reload
  edits under the MIR contract (the allocator never rewrites instruction
  semantics), GC-reference liveness per safepoint (handed to codegen's
  stack map finalizer);
- the determinism and replayability discipline (Rule 124) for the
  allocator's decisions;
- the NaN boxing representation contract surface (Part XVIII): if tagged
  values are enabled, the allocator must handle tagged-value register
  classes — a cross-team contract requiring this team's approval before
  enablement;
- the testing contract: liveness correctness vs the IR verifier's
  operand-type checks, GC-reference safety at safepoints, allocation
  determinism, spill correctness under deopt reconstruction.

---

## Dependencies

The regalloc team cannot ship until:

- T2 execution driver exists (lowers an IR graph to machine code or to
  T1 stencil plans). Today `compiler/codegen/src/T2Lowering.cpp` is a
  partial lowering not wired into any execution path; the `b2graph`
  tool dumps graphs only.
- The MIR contract between codegen and regalloc is pinned (the
  allocator emits spill/reload edits as MIR, never rewriting
  instruction semantics). The codegen team owns this seam.

Until then, this stub is the contract.

---

## Non-guarantees (v0)

- No allocation is performed. The T1 baseline (`compiler/baseline/`,
  `docs/baseline_contract.md`) does its own register handling under
  fixed conventions and does NOT consume the regalloc team's output
  (Amendment A: T1 is a no-IR tier).
- The interpreter (`compiler/interp/`) does not consume the regalloc
  team's output.
- The stack map format shipped by T1 (one bit per T0-frame slot, see
  `docs/baseline_contract.md` SS4) is independent of the regalloc
  team's per-safepoint reference-location data.

---

## Change control

Changes to this stub go through the message system under team key
`regalloc` (`docs/teams/messaging.md`). The v0 → v1 transition will be
announced with an INFO message and an accompanying ADVISORY to every
affected team (interpreter, baseline_noir, ir, passes, codegen, aot,
gc/runtime) before any allocator code lands.

Until v1 lands, no team may write to `compiler/regalloc/` or
`tests/regalloc/` — the paths do not exist; the integrator will either
add v0 stubs (empty `CMakeLists.txt` and a README pointing here) or
reconcile `docs/teams/ownership.yaml` to remove the paths until v1. That
decision is tracked in `docs/STATUS.md`'s "Recommended next steps" list,
item 2.
