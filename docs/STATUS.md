# B-2 Implementation Status

**Status:** Living document, integrator-maintained
**Owner:** B-2 integrator (`.github/`, top-level governance)
**Last Updated:** 2026-09-18
**Related Sections:** `docs/laws.md`, `docs/teams/ownership.yaml`, `README.md`

This document is the integrator's honest accounting of which parts of the
B-2 law system and architecture are mechanically realized in the tree today,
which are documented design only, and which are open work. It exists because
`docs/laws.md` and `README.md` make strong claims ("CI verifies compliance.
There are no exceptions.") that need a counterpart document that tells
adopters, contributors, and reviewers what is actually wired up.

If any statement here conflicts with `docs/laws.md`, `docs/laws.md` wins;
this document describes reality, not the target.

---

## What works today

The T0 / T1 / frontend / RBC path is real and exercised by `ctest`:

| Subsystem | Path | Status |
|---|---|---|
| Frontend (lexer, parser, AST, AST→RBC lowering) | `compiler/frontend/` | v1 landed; `--emit-rbc` closes source→RBC; loop heads emit `safepoint_poll` (form (a) of the baseline contract) |
| RBC (text format, builder, verifier, opcode table) | `compiler/rbc/` | Spec-conformant; the universal middle-end |
| IR (sea-of-nodes core: Graph, Verifier, Printer, NodeInfo, Serialize) | `compiler/ir/core/` | v2 core present; consumer-side lowering is partial |
| Passes (GraphBuilder, GVN, SCCP, Inline, Escape, DCE, Simplify, ControlFlow, PassSupport) | `compiler/passes/` | Pass bodies present and unit-tested; **no T2 execution driver** runs them on a real program |
| T0 interpreter (`b2run`) | `compiler/interp/` | 125 tests + 19-program corpus, all green |
| T1 baseline JIT (`b2jit`, copy-and-patch on x86-64) | `compiler/baseline/`, `compiler/codegen/` | **19/19 corpus programs compile to machine code** (was 17/19 before `MSG-20260918-001` relaxed the backedge-poll check to accept form (b); was 18/19 before `MSG-20260918-003` added the missing `fib_loop.rbc` poll) |
| Stencil archive (`tools/stencilgen/`) | `tools/stencilgen/` | Build-time generation; embedded into `b2jit` |
| Tests | `tests/{frontend,rbc,ir,passes,interp,baseline,codegen}/` | 10/10 ctest targets pass |

Entry paths exercised by `ctest`:
- Java source (`.java`) → AST → RBC → T0 interpreter, byte-identical to golden twins.
- RBC text (`.rbc`) → verifier hard-gate → T0 interpreter.
- RBC text (`.rbc`) → T1 plan → x86-64 machine code, byte-identical to T0 output (the differential law).

The README's claimed commands (`b2parse`, `b2rbc`, `b2plan`, `b2run`, `b2jit`) all
exist, build, and produce the documented behavior.

---

## What does NOT exist (despite docs or ownership)

| Claimed in / expected from | Path expected | Reality |
|---|---|---|
| `docs/teams/ownership.yaml` (gc team) | `compiler/gc/`, `include/b2/gc/`, `tests/gc/` | **No code.** `docs/gc.md` is design fiction. The interpreter uses a bump allocator (`compiler/interp/src/Heap.cpp`, 312 lines) with handles; no generational collector, no write barriers, no concurrent marking. |
| `docs/teams/ownership.yaml` (regalloc team) | `compiler/regalloc/`, `tests/regalloc/` | **No code, no team directory, no contract doc.** |
| `docs/teams/ownership.yaml` (aot team) | `compiler/aot/`, `tests/aot/` | **No code, no team directory, no contract doc.** T3 AOT is entirely missing. |
| `docs/teams/ownership.yaml` (passes team includes `compiler/pipeline/`) | `compiler/pipeline/` | **No such directory.** Pass implementations live in `compiler/passes/`. |
| `docs/laws.md` (Rule 11 / Rule 13: mutator threads never block on JIT; compiler threads never block on mutator state) | Async compilation, safepoint handshake protocol | **Not implemented.** `b2jit` is synchronous, single-threaded. No `std::thread` / `std::async` / `std::jthread` in `compiler/`. The laws' multi-threaded contracts are open work. |
| `docs/laws.md` Part I (T2 optimizing JIT) and `docs/codegen_contract.md` SS8 (T2 reuses the helper ABI) | T2 execution driver (IR → machine code lowering) | **Pass bodies exist but no driver runs them.** `compiler/codegen/src/T2Lowering.cpp` is a partial lowering that is not wired into any execution path; the `b2graph` tool dumps graphs only. |
| `docs/laws.md` Part I (T3 AOT) | T3 offline pipeline | **Not implemented.** |
| `docs/laws.md` (mentions `runtime/` tree) | `runtime/` | **No such directory.** The runtime seam lives in `compiler/interp/src/Runtime.cpp`. |
| Java classfile entry path (`Loader / Verifier / Quickener`) | A `.class` file loader | **Not implemented.** Only Java source entry works (`b2parse`). |
| Java standard library (`java.lang.*`, `java.io.*`, `java.util.*`) | Runtime stub for `java/lang/System.out.println(I)V` and a handful of others | **Stub only.** No `rt.jar` equivalent, no real classpath. Real Java programs that touch anything beyond the stubbed methods will not run. |
| Multi-architecture (ARM64, RISC-V) | Backend selection in `compiler/codegen/` | **x86-64 only.** Laws don't mention this. |
| Fuzzing harnesses | `fuzz/` or libFuzzer/AFL integrations | **None.** For a JIT that emits executable machine code, this is a serious gap. |
| JIT spraying / execute-only memory / `PKEY_MPROTECT` mitigations | Hardening in `compiler/codegen/` | **Not implemented.** W^X is honored (`mprotect` flips), but no execute-only memory, no MPK isolation, no constant-time blinding. |

---

## What is documented design only

These `docs/*.md` files describe subsystems that are not realized in the tree.
They are useful as design references and should not be deleted, but adopters
must not assume the described behavior is available:

- `docs/gc.md` — Generational Concurrent Region-Based Collector (no `compiler/gc/`).
- `docs/deopt_backend.md` — T2/T3 deopt backend (only T1 deopt is realized).
- `docs/special_passes.md` — CM-PEA escape lattice, speculative effect reordering, NaN boxing lowering. Pass bodies for `Escape` exist; PEA materialization, effect reordering, and NaN-boxed tagged-value nodes do not.
- `docs/icdg.md` — ICDG inline call/dispatch graph. Inline pass exists; the shared dispatch decision engine is not wired into a tier transition.
- `docs/inlining.md` — Inlining v1 (the inline pass is present and unit-tested; the budgets and refusal catalog are exercised in `tests/passes/InlineTests.cpp`).
- `docs/teams/regalloc-team.md`, `docs/teams/aot-team.md`, `docs/teams/gc-team.md` — team charters for teams with no code.

The integrator-owned `docs/teams/ownership.yaml` previously referenced
team contract docs that did not exist:
- `docs/baseline_contract.md` (referenced at `baseline_noir.write`) —
  **landed** as a v1 contract in `MSG-20260918-003`. Describes the
  stencil plan builder: inputs, the linear-scan algorithm, the budget
  (kill switch), the output plan, the backedge-poll membership check
  (Rule 88), refusals, determinism, telemetry, non-guarantees, future
  obligations.
- `docs/regalloc_contract.md` (referenced at `regalloc.write`) —
  **landed** as a v0 stub in `MSG-20260918-003`. Acknowledges no
  implementation; forward contract for what the regalloc team WILL ship
  when it ships, with dependencies documented.
- `docs/aot_contract.md` (referenced at `aot.write`) — **landed** as a
  v0 stub in `MSG-20260918-003`. Acknowledges no implementation; forward
  contract for the T2-pipeline-driven offline compiler (Amendment B.5).

The ownership map is now consistent with the tree for the contract doc
paths. The directory paths (`compiler/regalloc/`, `compiler/aot/`,
`compiler/gc/`, `compiler/pipeline/`, `runtime/`) are still missing;
the decision to add v0 stub directories or remove them from the map
until they ship is tracked in "Recommended next steps" item 2 below.

---

## CI and law compliance

`docs/laws.md` and `README.md` state "CI verifies compliance. There are no
exceptions." That claim is now mechanically real for the subset of
compliance that CI can check:

- `.github/workflows/ci.yml` — Release build + `ctest` on Ubuntu 24.04 with
  `g++-14`, plus a separate ASan + UBSan Debug build. Both run on every push
  and every pull request to `main`.
- `.github/CODEOWNERS` — surfaces the integrator-owned paths.

CI does NOT currently verify:
- the 150-rule law system itself (no mechanical checker exists for "every
  commit to `compiler/`, `runtime/`, `tools/`, and `tests/` complies");
- team ownership boundaries (`docs/teams/ownership.yaml` is not enforced);
- the Java spec compatibility contract (no JCK-equivalent suite);
- fuzzing (no harnesses; Rule 88 / safepoint stress live in `tests/` only);
- multi-architecture builds (single-arch x86-64 only).

These are open work tracked in this document, not in `messages/` (a P3 INFO
will accompany each future closing of one of these gaps).

---

## TurboScript sibling experiment

`turboscript/` is a separate JavaScript-engine-flavored sub-project (~9k
LOC, 97 test files, its own Makefile). It is:

- **NOT wired into the top-level `CMakeLists.txt`.** Build it separately
  with `make -C turboscript` per its own `Makefile`.
- **NOT mentioned in `README.md`.**
- **Governed by `docs/laws/turboscript_*.md`**, archived verbatim under
  owner-supplied directive (see `messages/open/MSG-20260912-001-integrator-all-INFO.md`).
  The B-2 law set (`docs/laws.md`) is normative for B-2 work; the TurboScript
  archive is normative for TurboScript work; neither subordinates the other.

TurboScript has no JS parser today; its `tsrun` consumes hand-assembled
`.tsbc` text bytecode. The interpreter is at v0.7
(`BumpArena<StringObj>` for string nodes) per the latest commits.

---

## Path ownership honesty

`docs/teams/ownership.yaml` lists nine teams and their `write:` paths. In
practice the repo has commits from a single AI-agent operator (the
"integrator"). The ownership map is the conceptual organization, not a
description of separate committers. When this document says "the
interpreter team" or "the codegen team", it means "the agent acting in
that role for that path", not a separate reviewer.

The path ownership map is also out of date in one place: it lists
`docs/baseline_contract.md`, `docs/regalloc_contract.md`, and
`docs/aot_contract.md` as team contract docs, but none of those files exist.
The integrator will either add stubs or reconcile the ownership map in a
follow-up.

---

## Recommended next steps (open work, ordered)

This list tracks the highest-leverage open work; it is not a roadmap.

1. ~~**Fix `fib_loop.rbc`** — add a `safepoint_poll` at the loop head so the
   backedge check accepts it; this closes the 18/19 → 19/19 corpus gap.
   Belongs to the interpreter team (their corpus file). Tracked as an open
   BUG message.~~ — **CLOSED in `MSG-20260918-003`**; corpus is 19/19.
2. **Reconcile `docs/teams/ownership.yaml`** with reality: either add
   `compiler/regalloc/`, `compiler/aot/`, `compiler/gc/`, `compiler/pipeline/`,
   `runtime/` directories with v0 stubs (an empty `CMakeLists.txt` and a
   README pointing at the team's contract doc), or remove them from the
   ownership map until they exist. Either choice is honest; the current
   state — paths claimed but missing — is not. The team contract docs
   (`docs/baseline_contract.md`, `docs/regalloc_contract.md`,
   `docs/aot_contract.md`) are now landed, so the contract doc layer is
   consistent; the directory layer is still inconsistent.
3. ~~**Add the missing contract docs** (`docs/baseline_contract.md`,
   `docs/regalloc_contract.md`, `docs/aot_contract.md`) or remove the
   references from the ownership map.~~ — **CLOSED in `MSG-20260918-003`**.
4. **T2 execution driver** — `compiler/passes/` has the pass bodies; a
   driver that lowers an IR graph to machine code (or to T1 stencil plans)
   is what unlocks the entire T2 tier. This is also the hard dependency
   for `compiler/regalloc/` (which cannot ship until the MIR contract
   exists) and `compiler/aot/` (which cannot ship until the T2 pipeline
   is wired to a driver).
5. **Multi-threaded compilation** — `b2jit` is synchronous today. Rules 11
   and 13 require async compilation with a safepoint handshake; this is the
   next big architecture piece after T2 lands.
6. **Real GC** — `compiler/interp/src/Heap.cpp` is a bump allocator. The
   `docs/gc.md` design is fine; implement it.
7. **Java standard library** — at minimum `java.lang.{Object,String,
   StringBuilder,Math,Integer,Long,Double}`, the `Exception` hierarchy,
   `java.io.PrintStream` / `System.out` / `System.err` properly. Without
   these, no real Java program runs.
8. **Java classfile loader** — the `.class` entry path is entirely missing.
9. **Fuzzing harnesses** for the RBC parser, RBC verifier, stencil
   instantiator, T1 execution, T0 interpreter.
10. **JIT hardening** — execute-only memory, JIT spraying mitigations,
    `PKEY_MPROTECT` where available.
11. **JCK-equivalent compatibility suite** — real Java programs, not
    hand-written `.rbc` fixtures.
12. **Second architecture** — ARM64 backend.

---

## Change log

| Date | Change |
|---|---|
| 2026-09-17 | Initial `STATUS.md` created by the integrator; accompanied by `.github/workflows/ci.yml`, `.github/CODEOWNERS`, the backedge-poll relaxation in `compiler/baseline/src/PlanBuilder.cpp`, and the corpus floor update in `tests/baseline/CorpusTest.cpp`. Corpus sweep moved from 17/19 to 18/19. |
| 2026-09-18 | `MSG-20260918-003` follow-up: `fib_loop.rbc` now carries a `safepoint_poll` at the loop head (form (a)), closing the corpus to 19/19. `tests/baseline/CorpusTest.cpp` floor `refused >= 1` removed (a refusal is now a regression, not a baseline). Three missing contract docs landed: `docs/baseline_contract.md` (v1), `docs/regalloc_contract.md` (v0 stub), `docs/aot_contract.md` (v0 stub). The path ownership map's contract-doc layer is now consistent with the tree; the directory layer (`compiler/regalloc/`, `compiler/aot/`, `compiler/gc/`, `compiler/pipeline/`, `runtime/`) is still missing. |
