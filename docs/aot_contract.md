# B-2 AOT Contract (v0 stub)

Owner: AOT Team (`aot`; see `docs/teams/aot-team.md`)

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
Amendment B.5: "AOT uses the T2 pipeline — not a separate optimizer."
```

**Status: v0 stub — no implementation.** `compiler/aot/`, `tests/aot/`,
and any AOT driver or artifact format do not exist in the tree today.
This document is the team's forward contract: it describes what the AOT
team WILL ship when it ships, what it will consume, and what it owes
consumers. The integrator's `docs/STATUS.md` carries the open-work
entry; this contract is the design hook for upstream and downstream
teams.

---

## Why this stub exists

`docs/teams/ownership.yaml` lists `compiler/aot/`, `tests/aot/`, and
`docs/aot_contract.md` as the AOT team's write paths. The charter
(`docs/teams/aot-team.md`) describes an offline T2-pipeline-driven AOT
compiler that emits manifests with mechanically-checked proofs. The
code is not yet written; the T2 pipeline it would reuse is also not yet
wired to a driver (see `docs/STATUS.md`). Rather than leave the contract
doc referenced but missing — which makes the ownership map and the
charter silently inconsistent — the integrator is landing this v0 stub
now.

When the AOT team ships, this stub will be replaced with a v1 contract
that pins:

- the AOT driver's inputs (the T2 pipeline run offline against closed
  world assumptions — `Amendment B.5: "AOT uses the T2 pipeline — not a
  separate optimizer"`);
- the AOT artifact format: the installable native code, the manifest
  with mechanically-checked proofs (the charter's deliverable 8: "whole
  program manifests and mechanically checked proofs"), the embedded
  deopt metadata that must round-trip into the T0 state contract
  (`docs/interp_contract.md`);
- the closure assumption: what the AOT path may assume (whole-program
  closed world, sealed class hierarchy, no reflection) and what it must
  refuse (dynamic class loading, reflection, `invokedynamic` bootstrap
  methods that escape closed world);
- the versioning and ABI stability story for AOT artifacts (a real
  concern for an AOT product that the v0 stub explicitly punts on);
- the determinism and replayability discipline (Rule 124) for the
  offline pipeline: identical source + identical target + identical
  flags → byte-identical artifact;
- the testing contract: AOT round-trip tests (compile offline, run,
  compare to T0 output), proof-checker tests for the mechanically
  checked proofs, manifest schema tests.

---

## Dependencies

The AOT team cannot ship until:

- T2 execution driver exists and the T2 pipeline is wired to a driver.
  Today `compiler/passes/` has pass bodies (GVN, SCCP, Inline, Escape,
  DCE, Simplify, GraphBuilder, ControlFlow, PassSupport) but no driver
  runs them on a real program; `compiler/codegen/src/T2Lowering.cpp` is
  a partial lowering not wired into any execution path; the `b2graph`
  tool dumps graphs only.
- The T2 pipeline is documented as stable enough for offline reuse
  (Amendment B.5: AOT does not fork the pipeline; it consumes the same
  passes the JIT runs).
- The deopt backend (`docs/deopt_backend.md`) is real — the AOT
  artifact's deopt metadata must round-trip into the T0 state contract,
  and the deopt backend is currently only realized for T1.

Until then, this stub is the contract.

---

## Non-guarantees (v0)

- No AOT compilation is performed. The `b2jit` driver (Tier 1,
  copy-and-patch) is the only path that produces machine code today,
  and it produces machine code at runtime, not offline.
- No AOT artifact format exists. The charter describes manifests with
  mechanically-checked proofs; the proof format, the manifest schema,
  and the install path are all unwritten.
- No closure analysis exists. The AOT team cannot reason about closed
  world assumptions until the loader / quickener stage exists (the
  `.class` entry path is entirely missing — see `docs/STATUS.md`).

---

## Change control

Changes to this stub go through the message system under team key `aot`
(`docs/teams/messaging.md`). The v0 → v1 transition will be announced
with an INFO message and an accompanying ADVISORY to every affected
team (interpreter, baseline_noir, ir, passes, regalloc, codegen,
gc/runtime) before any AOT code lands.

Until v1 lands, no team may write to `compiler/aot/` or `tests/aot/` —
the paths do not exist; the integrator will either add v0 stubs (empty
`CMakeLists.txt` and a README pointing here) or reconcile
`docs/teams/ownership.yaml` to remove the paths until v1. That decision
is tracked in `docs/STATUS.md`'s "Recommended next steps" list, item 2.
