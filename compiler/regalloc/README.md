# `compiler/regalloc/` — RegAlloc team (v0 stub)

**Status:** v0 stub. **No implementation.** See `docs/regalloc_contract.md`
and `docs/STATUS.md`.

This directory exists so `docs/teams/ownership.yaml` is consistent with
the tree: the regalloc team's write paths (`compiler/regalloc/`,
`tests/regalloc/`) resolve to real directories. The allocator code is
forward contract only today.

## Hard dependencies (cannot ship until)

- **T2 execution driver** — lowers an IR graph to machine code, then to
  the MIR the allocator consumes. Today
  `compiler/codegen/src/T2Lowering.cpp` is a partial lowering not wired
  into any execution path; the `b2graph` tool dumps graphs only.
- **MIR contract between codegen and regalloc** — the allocator emits
  spill/reload edits as MIR, never rewriting instruction semantics.
  The codegen team owns this seam.

## What lands here at v1

When the regalloc team ships, this directory will hold:

- liveness analysis over the machine-level representation produced by
  codegen lowering,
- live-interval construction and register assignment per register class,
- spill-slot layout and frame-size contribution,
- spill/reload edits under the MIR contract,
- GC-reference liveness per safepoint (handed to codegen's stack map
  finalizer),
- the determinism/replayability discipline (Rule 124),
- NaN-boxing tagged-value register class handling (Part XVIII),
  a cross-team contract requiring this team's approval before
  enablement,
- golden tests under `tests/regalloc/`.

The v0 -> v1 transition goes through the message system per
`docs/teams/messaging.md` and requires an ADVISORY to every affected
team (interpreter, baseline_noir, ir, passes, codegen, aot, gc/runtime)
before any allocator code lands.

## Non-guarantees (v0)

- No allocation is performed. The T1 baseline
  (`compiler/baseline/`) does its own register handling under fixed
  conventions and does NOT consume this team's output (Amendment A:
  T1 is a no-IR tier).
- The interpreter does not consume this team's output.
- The `b2::regalloc` CMake target is an INTERFACE library today;
  linking against it pulls in no objects.

## See also

- `docs/regalloc_contract.md` — v0 contract (forward contract only)
- `docs/teams/regalloc-team.md` — team charter
- `docs/STATUS.md` — open-work entry
