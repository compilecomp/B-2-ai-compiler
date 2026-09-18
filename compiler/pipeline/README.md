# `compiler/pipeline/` — passes team (v0 stub)

**Status:** v0 stub. **No implementation.** See `docs/STATUS.md` and
`docs/pass_contracts.md`.

This directory exists so `docs/teams/ownership.yaml` is consistent with
the tree: the passes team's `compiler/pipeline/` write path resolves to
a real directory. The actual pass bodies (GVN, SCCP, Inline, Escape,
DCE, Simplify, ControlFlow, PassSupport, GraphBuilder) live in
`compiler/passes/` today; the pipeline orchestrator that orders them,
manages budgets, and lowers IR to machine code is not yet written.

## Hard dependencies (cannot ship until)

- **T2 execution driver** — the pipeline orchestrator drives the T2
  pipeline against a real program. Today the driver does not exist;
  `compiler/codegen/src/T2Lowering.cpp` is a partial lowering not wired
  into any execution path; the `b2graph` tool dumps graphs only.

## What lands here at v1

When the passes team ships the pipeline, this directory will hold:

- the pipeline orchestrator (orders passes, manages budgets, runs the
  pass manager),
- the T2 execution driver (lowers IR graphs to machine code or to T1
  stencil plans),
- the deopt wiring (hands FrameState + stack maps to the deopt backend),
- the tier transition logic (T0 -> T1 -> T2 with PGO, deopt, reprofiling),
- golden tests under `tests/passes/` (extend the existing suite).

The v0 -> v1 transition goes through the message system per
`docs/teams/messaging.md` and requires an ADVISORY to every affected
team (interpreter, baseline_noir, ir, codegen, regalloc, aot,
gc/runtime) before any pipeline code lands.

## Non-guarantees (v0)

- No pipeline is run. The pass bodies in `compiler/passes/` are
  unit-tested in isolation but never run on a real program.
- The `b2::pipeline` CMake target is an INTERFACE library today;
  linking against it pulls in no objects.

## See also

- `docs/pass_contracts.md` — pass contracts
- `docs/special_passes.md` — special pass designs (CM-PEA, effect
  reordering, adaptive value representation)
- `docs/icdg.md` — inline call/dispatch graph contract
- `docs/STATUS.md` — open-work entry on T2 execution driver
- `docs/teams/passes-team.md` — team charter
