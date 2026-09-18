# `compiler/aot/` — AOT team (v0 stub)

**Status:** v0 stub. **No implementation.** See `docs/aot_contract.md`
and `docs/STATUS.md`.

This directory exists so `docs/teams/ownership.yaml` is consistent with
the tree: the aot team's write paths (`compiler/aot/`, `tests/aot/`)
resolve to real directories. T3 AOT is entirely missing today.

## Hard dependencies (cannot ship until)

- **T2 execution driver** — AOT uses the T2 pipeline, not a separate
  optimizer (Amendment B.5). Today the T2 driver does not exist.
- **Deopt backend** — `docs/deopt_backend.md` describes T2/T3 deopt
  metadata; only T1 deopt is realized today (`compiler/baseline/`,
  `compiler/codegen/`).
- **Closure analysis** — AOT requires a closed-world assumption; a
  reachable-methods analysis must exist.

## What lands here at v1

When the aot team ships, this directory will hold:

- the offline driver that runs the T2 pipeline against closed-world
  assumptions (whole-program analysis),
- the manifest emitter with mechanically-checked proofs
  (Amendment B.5 form: each compiled method carries a proof of
  correctness against the law system),
- the AOT object file format (`.b2o`) and the link/loader for
  precompiled images,
- deopt backend wiring for AOT-compiled code,
- golden tests under `tests/aot/`.

The v0 -> v1 transition goes through the message system per
`docs/teams/messaging.md` and requires an ADVISORY to every affected
team (interpreter, baseline_noir, ir, passes, codegen, regalloc,
gc/runtime) before any AOT code lands.

## Non-guarantees (v0)

- No offline compilation is performed. Only the T0 interpreter
  (`b2run`) and the T1 baseline JIT (`b2jit`) produce executable code
  today.
- The `b2::aot` CMake target is an INTERFACE library today; linking
  against it pulls in no objects.

## See also

- `docs/aot_contract.md` — v0 contract (forward contract only)
- `docs/teams/aot-team.md` — team charter
- `docs/STATUS.md` — open-work entry on T3 AOT
