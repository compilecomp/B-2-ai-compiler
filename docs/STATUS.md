# B-2 Implementation Status

**Status:** Living document, integrator-maintained
**Owner:** B-2 integrator (`.github/`, top-level governance)
**Last Updated:** 2026-09-18 (rev 5)
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
| Passes (GraphBuilder, GVN, SCCP, Inline, Escape, DCE, Simplify, ControlFlow, PassSupport) | `compiler/passes/` | Pass bodies present and unit-tested; the T2 driver runs them on real programs when `-O` is passed (off by default; opt-mode divergences tracked as `MSG-20260918-006` item 4) |
| T0 interpreter (`b2run`) | `compiler/interp/` | 125 tests + 19-program corpus, all green |
| T1 baseline JIT (`b2jit`, copy-and-patch on x86-64) | `compiler/baseline/`, `compiler/codegen/` | **19/19 corpus programs compile to machine code** (was 17/19 before `MSG-20260918-001` relaxed the backedge-poll check to accept form (b); was 18/19 before `MSG-20260918-003` added the missing `fib_loop.rbc` poll) |
| T2 execution driver (`b2t2`, IR → x86-64 via `lowerOnly`) | `compiler/codegen/tools/b2t2.cpp`, `compiler/codegen/src/T2Lowering.cpp` | **v0 wired end-to-end in `MSG-20260918-005`**: parses + verifies + builds IR + lowers + installs + executes. Differential (Rule 36 form) holds for **16/19** corpus programs in the default no-opt config; 3 known no-opt divergences + 1 opt-mode divergence tracked as `MSG-20260918-006`. The v0 is NOT yet a perf win (lowering traps early on most programs; engine deopt-to-T0 catches the trap; observable behavior preserved). v0 → v1 transition items in `docs/t2_driver_contract.md`. |
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
| `docs/teams/ownership.yaml` (gc team) | `compiler/gc/`, `include/b2/gc/`, `tests/gc/` | **v0 stub directories landed** in `MSG-20260918-004` (INTERFACE library + README pointing at the contract). **No code.** `docs/gc.md` is design fiction. The interpreter uses a bump allocator (`compiler/interp/src/Heap.cpp`, 312 lines) with handles; no generational collector, no write barriers, no concurrent marking. |
| `docs/teams/ownership.yaml` (regalloc team) | `compiler/regalloc/`, `tests/regalloc/` | **v0 stub directories landed** in `MSG-20260918-004` (INTERFACE library + README pointing at the contract). **No allocator code.** The v0 contract is `docs/regalloc_contract.md`; the v1 implementation is blocked on the T2 execution driver and the MIR contract between codegen and regalloc. |
| `docs/teams/ownership.yaml` (aot team) | `compiler/aot/`, `tests/aot/` | **v0 stub directories landed** in `MSG-20260918-004` (INTERFACE library + README pointing at the contract). **No AOT code.** The v0 contract is `docs/aot_contract.md`; the v1 implementation is blocked on the T2 driver, the deopt backend, and closure analysis. |
| `docs/teams/ownership.yaml` (passes team includes `compiler/pipeline/`) | `compiler/pipeline/` | **v0 stub directory landed** in `MSG-20260918-004` (INTERFACE library + README). **No pipeline orchestrator code.** Pass implementations live in `compiler/passes/`; the pipeline driver that orders them is blocked on the T2 execution driver. |
| `docs/laws.md` (Rule 11 / Rule 13: mutator threads never block on JIT; compiler threads never block on mutator state) | Async compilation, safepoint handshake protocol | **Not implemented.** `b2jit` is synchronous, single-threaded. No `std::thread` / `std::async` / `std::jthread` in `compiler/`. The laws' multi-threaded contracts are open work. |
| `docs/laws.md` Part I (T2 optimizing JIT) and `docs/codegen_contract.md` SS8 (T2 reuses the helper ABI) | T2 execution driver (IR → machine code lowering) | **Driver wired in `MSG-20260918-005`** (`b2t2`, `compiler/codegen/tools/b2t2.cpp`): parses + verifies + builds IR + (optionally) runs opt pipeline + lowers via `codegen::lowerOnly` + installs on `Tier1` engine + executes. Differential (Rule 36 form) holds for **16 of 19** corpus programs in the default no-opt config; 3 known no-opt divergences + 1 opt-mode divergence tracked as `MSG-20260918-006`. The v0 is NOT yet a perf win (the lowering traps early on most programs; the engine deopt-to-T0 path catches the trap; observable behavior preserved). The opt-mode (`-O`) is off by default. The v0 → v1 transition items (real regalloc, async, T2/T3 deopt backend, opt-mode default ON) are documented in `docs/t2_driver_contract.md`. |
| `docs/laws.md` Part I (T3 AOT) | T3 offline pipeline | **Not implemented.** |
| `docs/laws.md` (mentions `runtime/` tree) | `runtime/` | **No such directory.** The runtime seam lives in `compiler/interp/src/Runtime.cpp`. `runtime/` is a target layout referenced in `docs/laws.md` but not in `docs/teams/ownership.yaml`; intentionally not added as a stub. |
| Java classfile entry path (`Loader / Verifier / Quickener`) | A `.class` file loader | **Not implemented.** Only Java source entry works (`b2parse`). |
| Java standard library (`java.lang.*`, `java.io.*`, `java.util.*`) | Runtime stub for `java/lang/System.out.println(I)V` and a handful of others | **Stub only.** No `rt.jar` equivalent, no real classpath. Real Java programs that touch anything beyond the stubbed methods will not run. |
| Multi-architecture (ARM64, RISC-V) | Backend selection in `compiler/codegen/` | **x86-64 only.** Laws don't mention this. |
| Fuzzing harnesses | `fuzz/` or libFuzzer/AFL integrations | **Scaffold landed** in `MSG-20260918-004`: `fuzz/rbc_text_fuzzer.cpp` (RBC text parser) and `fuzz/rbc_verifier_fuzzer.cpp` (RBC verifier), opt-in via `B2_BUILD_FUZZERS=ON` (clang + `-fsanitize=fuzzer`). Seeded from `tests/rbc/corpus/`. **CI does not yet run them** (default toolchain is g++-14); **T1/T0/stencil harnesses not yet landed** (blocked on teardown plumbing and per-run heap reset). |
| JIT spraying / execute-only memory / `PKEY_MPROTECT` mitigations | Hardening in `compiler/codegen/` | **W^X honored** today (every JIT buffer goes through `mprotect` flips: never writable-while-executable, see `compiler/codegen/src/Instantiate.cpp` lines 843-878 and `compiler/codegen/src/T2Lowering.cpp` lines 1239-1252). **No execute-only memory, no MPK isolation, no constant-time blinding, no JIT spraying mitigations.** The v1 plan is documented in `docs/jit_hardening.md` (landed `MSG-20260918-004`); the code is not yet landed. |

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

The ownership map is now consistent with the tree at both layers:
contract-doc layer (closed in `MSG-20260918-003`) and directory layer
(closed in `MSG-20260918-004`, see "Recommended next steps" item 2).
The `runtime/` directory is intentionally not added — it is a target
layout referenced in `docs/laws.md` but not in `docs/teams/ownership.yaml`;
the runtime seam today lives in `compiler/interp/src/Runtime.cpp`.

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

The path ownership map is now consistent with the tree at both layers:
the contract-doc layer landed in `MSG-20260918-003`
(`docs/baseline_contract.md`, `docs/regalloc_contract.md`,
`docs/aot_contract.md`); the directory layer landed in `MSG-20260918-004`
(`compiler/gc/`, `compiler/regalloc/`, `compiler/aot/`, `compiler/pipeline/`,
`include/b2/gc/`, `tests/gc/`, `tests/regalloc/`, `tests/aot/` — each with a
v0 stub `CMakeLists.txt` declaring an INTERFACE library and a README
pointing at the contract doc). The `runtime/` directory is still missing —
it is not in the ownership map (the runtime seam lives in
`compiler/interp/src/Runtime.cpp`); only `docs/laws.md` references it as a
future tree. That reference is acknowledged in `docs/laws.md` as a target
layout, not a current one.

---

## Recommended next steps (open work, ordered)

This list tracks the highest-leverage open work; it is not a roadmap.

1. ~~**Fix `fib_loop.rbc`** — add a `safepoint_poll` at the loop head so the
   backedge check accepts it; this closes the 18/19 → 19/19 corpus gap.
   Belongs to the interpreter team (their corpus file). Tracked as an open
   BUG message.~~ — **CLOSED in `MSG-20260918-003`**; corpus is 19/19.
2. ~~**Reconcile `docs/teams/ownership.yaml`** with reality: either add
   `compiler/regalloc/`, `compiler/aot/`, `compiler/gc/`, `compiler/pipeline/`,
   `runtime/` directories with v0 stubs (an empty `CMakeLists.txt` and a
   README pointing at the team's contract doc), or remove them from the
   ownership map until they exist. Either choice is honest; the current
   state — paths claimed but missing — is not.~~ — **CLOSED in
   `MSG-20260918-004`**: v0 stub directories landed for `compiler/gc/`,
   `compiler/regalloc/`, `compiler/aot/`, `compiler/pipeline/`,
   `include/b2/gc/`, `tests/gc/`, `tests/regalloc/`, `tests/aot/`. The
   `runtime/` directory is not in `ownership.yaml`; the runtime seam lives
   in `compiler/interp/src/Runtime.cpp` and is acknowledged there.
3. ~~**Add the missing contract docs** (`docs/baseline_contract.md`,
   `docs/regalloc_contract.md`, `docs/aot_contract.md`) or remove the
   references from the ownership map.~~ — **CLOSED in `MSG-20260918-003`**.
4. **T2 execution driver** — `compiler/passes/` has the pass bodies; a
   driver that lowers an IR graph to machine code (or to T1 stencil plans)
   is what unlocks the entire T2 tier. This is also the hard dependency
   for `compiler/regalloc/` (which cannot ship until the MIR contract
   exists) and `compiler/aot/` (which cannot ship until the T2 pipeline
   is wired to a driver).
   - **Driver wired end-to-end in `MSG-20260918-005`**: `b2t2`
     (the T2 driver surface, `compiler/codegen/tools/b2t2.cpp`) parses +
     verifies RBC + builds the sea-of-nodes IR (`passes::buildGraph`) +
     (optionally, default OFF) runs the optimization pipeline +
     lowers via `codegen::lowerOnly` + installs the `CompiledCode` on a
     `Tier1` engine + executes the entry method. The differential
     contract (Rule 36 form: T2 byte-identical to T0 on the corpus)
     holds for **16 of 19** programs in the default no-opt config;
     the 3 known divergences (`conversions.rbc`, `fields.rbc`,
     `float_math.rbc`) plus the opt-mode divergence on
     `strings_intern.rbc` are tracked as `MSG-20260918-006`. The v0
     driver is wired, the safety holds, but the lowering traps early
     on most programs (the engine deopt-to-T0 path catches the trap;
     observable behavior is preserved, but the v0 is NOT yet a perf
     win). The v0 → v1 transition items are documented in
     `docs/t2_driver_contract.md` (real regalloc, async compilation,
     T2/T3 deopt backend, opt-mode default ON once item 4 in
     `MSG-20260918-006` is fixed).
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
   - **Scaffold landed in `MSG-20260918-004`** for two of five entry
     points: `fuzz/rbc_text_fuzzer.cpp` (RBC text parser) and
     `fuzz/rbc_verifier_fuzzer.cpp` (RBC verifier). Opt-in via
     `B2_BUILD_FUZZERS=ON` (clang + `-fsanitize=fuzzer`). Seeded from
     `tests/rbc/corpus/`. CI does not yet run them (default toolchain
     is g++-14). The remaining three entry points (T1 instantiation,
     T0 interpreter, stencil instantiator) are blocked on plumbing
     teardown paths to the fuzzer's exit (T1) and a per-run heap reset
     API (T0); both are tracked as v1 work in `fuzz/README.md`.
10. **JIT hardening** — execute-only memory, JIT spraying mitigations,
    `PKEY_MPROTECT` where available.
    - **Design doc landed in `MSG-20260918-004`**: `docs/jit_hardening.md`
      describes the v0 (W^X is honored today, every other mitigation is
      open work) vs v1 (execute-only memory via MPK, constant blinding,
      per-thread PKRU isolation, constant-time mode, resource limits)
      plan. The hardening code itself is not yet landed; the design doc
      is the honest contract for what the JIT does today and what it
      will do when the v1 work ships.
11. **JCK-equivalent compatibility suite** — real Java programs, not
    hand-written `.rbc` fixtures.
12. **Second architecture** — ARM64 backend.
13. **Partial deopt (dependency-driven invalidation + minimal safe region
    recompilation + atomic swap)** — the v1 layer on top of the v0
    method-granularity invalidation (`docs/deopt_backend.md` Section 17).
    When a T2 guard fails, the v0 does full method deopt to T0 (Rule 96
    form, the existing path); the v1 attempts to salvage the speculation
    by rebuilding only the smallest safe recompilation region containing
    the broken assumption's dirty closure, keeping unaffected optimized
    code alive. The IR already carries the compile-time scaffolding
    (`ir::Dependency`, `ir::SpecMeta.dependency`, `ir::FrameStateDesc`,
    `ir::Replacement`); the v0 contract surfaces are landed in
    `include/b2/pipeline/` (`DependencyIndex.h`, `Region.h`,
    `PartialDeopt.h`, `DirtyClosure.h`) and `docs/partial_deopt_contract.md`
    (25 sections mirroring the design: dependency index, region structure,
    dirty-node closure, region safety checks, partial rebuild, atomic
    activation, deopt budget + hysteresis, verification gates, feature
    flags, telemetry, dangerous cases, the production-safety principle
    "partial deopt must never be required for correctness"). The v0.1
    implementation of the dirty-node closure algorithm (Section 5) landed
    in `MSG-20260918-008` (`compiler/pipeline/src/DirtyClosure.cpp`): the
    forward closure walks def-use chains through Data/Mem/FrameState/Parent
    roles; side-effecting users (Call/Memory/Guard) always propagate dirty;
    the budget cap is enforced; the algorithm is deterministic (Rule 124).
    12 unit tests in `tests/pipeline/DirtyClosureTests.cpp`. The backward
    closure (region expansion through boundary nodes) requires the region
    builder, which is open work. The v0 ships shadow-only:
    `enable_partial_deopt = false` is the default; the engine's trap
    handler in `compiler/codegen/src/Engine.cpp` `executeCompiled`
    short-circuits every call to `onGuardFailure()` and goes straight to
    the existing T0 deopt path. The v0 → v1 transition implements the
    bodies; the v1 default flips to `true` once the shadow comparison
    (compute partial plan, verify, do not activate, compare with full
    deopt behavior) passes the corpus. The hard dependencies are the T2
    driver (landed in `MSG-20260918-005`) and the multi-threaded
    compilation base (`docs/STATUS.md` item 5; the atomic swap needs the
    safepoint handshake protocol, Rules 11/13).

---

## Change log

| Date | Change |
|---|---|
| 2026-09-17 | Initial `STATUS.md` created by the integrator; accompanied by `.github/workflows/ci.yml`, `.github/CODEOWNERS`, the backedge-poll relaxation in `compiler/baseline/src/PlanBuilder.cpp`, and the corpus floor update in `tests/baseline/CorpusTest.cpp`. Corpus sweep moved from 17/19 to 18/19. |
| 2026-09-18 | `MSG-20260918-003` follow-up: `fib_loop.rbc` now carries a `safepoint_poll` at the loop head (form (a)), closing the corpus to 19/19. `tests/baseline/CorpusTest.cpp` floor `refused >= 1` removed (a refusal is now a regression, not a baseline). Three missing contract docs landed: `docs/baseline_contract.md` (v1), `docs/regalloc_contract.md` (v0 stub), `docs/aot_contract.md` (v0 stub). The path ownership map's contract-doc layer is now consistent with the tree; the directory layer (`compiler/regalloc/`, `compiler/aot/`, `compiler/gc/`, `compiler/pipeline/`, `runtime/`) is still missing. |
| 2026-09-18 | `MSG-20260918-004` follow-up: v0 stub directories landed for `compiler/gc/`, `compiler/regalloc/`, `compiler/aot/`, `compiler/pipeline/`, `include/b2/gc/`, `tests/gc/`, `tests/regalloc/`, `tests/aot/` — each with a stub `CMakeLists.txt` declaring an INTERFACE library and a README pointing at the team's contract doc. Top-level `CMakeLists.txt` wires them in unconditionally so the ownership map is consistent with the tree. The `runtime/` directory is intentionally not added (it is not in `ownership.yaml`; the runtime seam lives in `compiler/interp/src/Runtime.cpp`). Fuzzing scaffold landed for the RBC text parser (`fuzz/rbc_text_fuzzer.cpp`) and RBC verifier (`fuzz/rbc_verifier_fuzzer.cpp`); opt-in via `B2_BUILD_FUZZERS=ON`; hard configure error without a `-fsanitize=fuzzer`-capable toolchain. Seeded from `tests/rbc/corpus/`. JIT hardening design doc landed at `docs/jit_hardening.md` — describes the v0 (W^X) vs v1 (execute-only, constant blinding, MPK, constant-time, resource limits) plan; no hardening code lands in this commit, only the contract. Build + ctest verified 19/19 corpus + 10/10 ctest targets pass after the directory additions. |
| 2026-09-18 | `MSG-20260918-005`: T2 execution driver landed (`b2t2`, `compiler/codegen/tools/b2t2.cpp`). Wires the full RBC -> IR -> machine-code -> execute path end-to-end: parse + verify + build IR (`passes::buildGraph`) + (optionally, default OFF) run opt pipeline + lower via `codegen::lowerOnly` + install on `Tier1` engine + execute. The differential (Rule 36 form: T2 byte-identical to T0 on the corpus) holds for **16 of 19** programs in the default no-opt config; the 3 known no-opt divergences (`conversions.rbc`, `fields.rbc`, `float_math.rbc`) + 1 opt-mode divergence on `strings_intern.rbc` are tracked as `MSG-20260918-006`. The v0 is wired end-to-end and differentially safe on 16/19, but the lowering traps early on most programs (the engine deopt-to-T0 path catches the trap; observable behavior preserved; v0 is NOT yet a perf win). The v0 → v1 transition items (real regalloc, async, T2/T3 deopt backend, opt-mode default ON) are documented in `docs/t2_driver_contract.md`. New test `tests/codegen/T2CorpusTest.cpp` skips the 3 known no-opt bugs in `kKnownBugs`; the rest of the corpus remains a regression gate. ASan + UBSan clean. The T2 driver unblocks `regalloc/` v1 and `aot/` v1 (both depend on the T2 driver existing). |
| 2026-09-18 | `MSG-20260918-007`: Partial deopt v0 contract landed (`docs/partial_deopt_contract.md`, 25 sections mirroring the design: dependency index, region structure, dirty-node closure, region safety checks, partial rebuild, atomic activation, deopt budget + hysteresis, verification gates, feature flags, telemetry, dangerous cases, the production-safety principle "partial deopt must never be required for correctness"). The IR already carries the compile-time scaffolding (`ir::Dependency`, `ir::SpecMeta.dependency`, `ir::FrameStateDesc`); the v0 contract surfaces are landed in `include/b2/pipeline/` (`DependencyIndex.h`, `Region.h`, `PartialDeopt.h`) — headers-only, no `.cpp` files, every function a no-op (returns `Disabled` / empty sets). `compiler/pipeline/CMakeLists.txt` declares `b2::pipeline` as an INTERFACE library linking `b2::ir` so consumers get the include path transitively. `docs/deopt_backend.md` Section 17 cross-references the partial deopt contract. The v0 ships shadow-only: `enable_partial_deopt = false` is the default; the engine's trap handler in `compiler/codegen/src/Engine.cpp` `executeCompiled` short-circuits every call to `onGuardFailure()` and goes straight to the existing T0 deopt path. New open-work item 13 added to "Recommended next steps". The hard dependencies are the T2 driver (landed in `MSG-20260918-005`) and the multi-threaded compilation base (`docs/STATUS.md` item 5; the atomic swap needs the safepoint handshake protocol, Rules 11/13). Build + 10/10 ctest targets pass after the contract surface additions (no behavior change; the v0 is shadow-only). |
| 2026-09-18 | `MSG-20260918-008`: Partial deopt v0.1 — dirty-node closure algorithm implemented (`compiler/pipeline/src/DirtyClosure.cpp` + `include/b2/pipeline/DirtyClosure.h`). The forward closure walks def-use chains through `InputRole::Data/Mem/FrameState/Parent` (Ctrl and None do NOT propagate); side-effecting users (`NodeClass::Call/Memory` non-Pure / `NodeClass::Guard`) ALWAYS propagate dirty regardless of the input slot's role (Section 13: "If a dirty region contains calls/stores/allocations, be conservative"). The `semanticsDependOn` predicate is exposed for unit tests + the v0 → v1 transition's relaxation. The budget cap is enforced: if the closure exceeds `max_dirty_region_size` (default 64; `PartialDeoptConfig::max_dirty_region_size`), the algorithm sets `DirtyClosureResult::budget_exceeded = true` and stops; the caller escalates to full method deopt. 12 unit tests in `tests/pipeline/DirtyClosureTests.cpp` (empty seed, seed dedup, dead-seed drop, Data/Mem/Parent propagation, Ctrl no-propagation, side-effecting-user rule, budget cap, determinism, the `semanticsDependOn` predicate). `b2_pipeline` is now a STATIC library (was INTERFACE in v0). New ctest target `pipeline_tests` (the 12th test, 12/12 pass). Build + ctest verified 19/19 T1 corpus + 16/19 T2 differential + 12/12 pipeline tests pass; ASan+UBSan clean. The backward closure (region expansion through boundary nodes) requires the region builder, which is open work. The v0 → v1 transition items: region builder, region safety checks, partial rebuild, atomic activation, the guard-failure integration (the engine's trap handler calling `onGuardFailure()`). |
