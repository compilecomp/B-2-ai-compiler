# B-2 T2 Execution Driver Contract (v0)

Owner: Codegen Team (`codegen`; see `docs/teams/codegen-team.md`) +
Passes Team (`passes`; the pipeline the driver optionally runs) +
Integrator (the `b2t2` driver surface)

```text
Normative reference: docs/laws.md
If this document conflicts with docs/laws.md, docs/laws.md wins.
```

**Status: v0 — driver wired end-to-end; optimization off by default.** The
T2 execution driver (`b2t2`) parses an RBC text program, verifies it, builds
every method's sea-of-nodes IR graph, optionally runs the optimization
pipeline, lowers each graph to x86-64 machine code, installs the resulting
`CompiledCode` on a `Tier1` engine, and executes the entry method. The
differential contract (Rule 36 form) holds in the default (no-opt)
configuration for 16 of the 19 interpreter-corpus programs; the 3 known
divergences are tracked as `MSG-20260918-006` (no-opt) and the opt-mode
divergence on `strings_intern.rbc` is tracked in the same message.

---

## What is wired today

### The driver (`compiler/codegen/tools/b2t2.cpp`)

`b2t2` is a sibling tool to `b2jit` (T1) and `b2graph` (IR builder debug
surface). It mirrors `b2jit`'s role for T2:

| Phase | T1 (`b2jit`) | T2 (`b2t2`) |
|---|---|---|
| Parse + verify RBC | yes | yes (same) |
| Build IR graph | no | yes (`passes::buildGraph`) |
| Run optimization pipeline | no | optional (`-O`: `runInlining` + `runEarlyCleanup` + `runPartialEscapeAnalysis`) |
| Lower to machine code | stencil plan + instantiate (`baseline::compilePlan` + `codegen::instantiate`) | IR → x86-64 (`codegen::lowerOnly` in `compiler/codegen/src/T2Lowering.cpp`) |
| Install + execute | `Tier1::run` | `Tier1::run` (after `installCompiledCode`; same engine, same helpers, same deopt-to-T0 path) |
| Differential safety | Rule 36 form (byte-identical to `b2run` on the corpus) | Rule 36 form, default no-opt, 16/19 today (3 known bugs in `MSG-20260918-006`) |

`b2t2` links the same libraries `b2jit` links plus `b2::passes` (the T2
pipeline: `buildGraph` + `runEarlyCleanup` + `runInlining` +
`runPartialEscapeAnalysis`) and `b2::ir` (the verifier the driver runs
between passes per Rule 40).

### The lowering (`compiler/codegen/src/T2Lowering.cpp`)

`lowerOnly` takes a verified `ir::Graph` + the `rbc::Program` + the
`rbc::Method` + a `Runtime&` and returns a `std::unique_ptr<CompiledCode>`
(or `nullptr` with a refusal reason). The lowering:

- assigns every value node a 16-byte activation slot (`assignSlots`),
- optionally assigns a Phi at a `LoopBegin` to a register for the loop's
  hot variable (`assignRegisters`),
- emits x86-64 bytes for every block: phi moves, value nodes
  (`ConstantI` → `mov eax, imm; mov [rbp+slot], eax`, etc.), the
  terminator (`If` → `cmp; jcc`, `Goto` → `jmp`, `Return` → mov rax;
  leave; ret),
- helper calls for side-effecting nodes (`CallVirtual` → `b2cg_call`,
  `New` → `b2cg_new_object`, `LoadField` → `b2cg_get_field`, etc.),
- publishes the bytes W^X (`mprotect` flips; `Stencil Rule 5` form;
  see `docs/jit_hardening.md` for the v0 W^X state).

The compiled code is installed on the `Tier1` engine via
`Tier1::installCompiledCode`. When `Tier1::run` looks up the cache for
the entry method, it finds the T2-installed code and executes it via
`executeCompiled`. If the T2 code traps (e.g., an unsupported opcode
pattern), the engine's deopt path catches the trap and re-executes the
method via T0 (`Rule 96`: the plan is a cache, not a correctness claim).

### The differential (`tests/codegen/T2CorpusTest.cpp`)

The `codegen_t2_corpus_differential` test runs every `.rbc` in the
interpreter corpus through the T2 driver (no opt) and compares the
output + status to the T0 golden twin (`.expected`). The 3 known
divergences are listed in `kKnownBugs` in the test file; the test SKIPS
them (does not FAIL on them) so the rest of the corpus remains a
meaningful regression gate. When a bug is fixed, the entry is removed
from `kKnownBugs`; the test then enforces the program against the T0
golden twin.

The test also asserts:
- `ran >= 16` — the corpus was swept (19 programs minus 3 known bugs),
- `passed == ran` — every swept program produced byte-identical T0
  output,
- `lowered >= 16` — every swept program had at least one method
  T2-lowered (the driver actually ran machine code, not a silent T0
  fallback for the whole corpus),
- `skipped == kKnownBugs.size()` — the skip list is not stale (a
  fixed bug without removing the entry is caught).

---

## What is NOT wired (open work)

### Optimization is off by default

The T2 driver's default is NO optimization (`-O` opts in). The
optimization pipeline (`runInlining` + `runEarlyCleanup` +
`runPartialEscapeAnalysis`) is known to diverge from T0 on at least one
program (`strings_intern.rbc`: the SCCP-folded graph produces no output
because the T2 lowering does not correctly handle the post-opt shape of
the `CallVirtual`'s arg list). This is tracked as `MSG-20260918-006`
item 4.

The opt pipeline is the most leveraged open work: it is what makes T2
actually worth running (vs. T0/T1). The opt-mode divergences are the
v0 → v1 transition's blockers.

### The T2 lowering traps early on most programs

In the v0 no-opt configuration, the T2 lowering produces machine code
that traps on the first side-effecting operation for most corpus
programs. The trap is caught by the engine's deopt path, which
re-executes the method via T0; observable behavior is preserved (the
differential holds), but no actual T2-native execution happens for
the program's main loop body.

This is honest: the driver is wired, the safety holds, but the v0 is
not yet a performance win. The v0 → v1 transition must:

- fix the lowering patterns that currently trap (the first
  side-effecting opcode in many programs: `getstatic r0 c0` for
  `System.out`, the `ldc` of a string constant, etc.),
- wire the helper-call path for `CallVirtual` so it does not trap on
  the receiver null-check (`b2cg_call` already does the null check; the
  lowering's `Guard` before the call may be the trap site),
- measure the T2 vs T1 vs T0 steady-state performance on the bench
  corpus (`compiler/interp/tools/b2bench.cpp`).

### Real register allocation

Today the T2 lowering assigns one register (EAX) for hot Phis at
`LoopBegin` and spills everything else to slots. This is correct (the
differential holds) but slow (most operations are load-compute-store
through memory). A real register allocator (`compiler/regalloc/`, the
v0 stub landed in `MSG-20260918-004`) is the v1 hard dependency for any
performance claim.

### Multi-threaded compilation

`b2t2` is synchronous today (the same gap as `b2jit`). Rules 11 and 13
require async compilation with a safepoint handshake; this is the next
big architecture piece after T2 lands (see `docs/STATUS.md` item 5).

### Deopt backend

The T2 path reuses the T1 deopt backend (the `executeCompiled` trap
handler in `compiler/codegen/src/Engine.cpp` reconstructs a T0 `Frame`
from the activation's slots + the `CompiledCode`'s `pc_map` + the
`deopt_points`). This works for the v0 because the T2 lowering's
FrameState discipline matches the T1/T0 frame shape. The T2/T3 deopt
backend (`docs/deopt_backend.md`) is design-only today; the v0
operationally reuses T1 deopt.

---

## Threat model

The T2 driver's threat model is the same as T1's
(`docs/jit_hardening.md`):

- The RBC program text is attacker-controlled.
- The RBC verifier is the hard gate.
- The host process's heap, stack, and code segments are not directly
  attacker-writable.
- The attacker's goal is arbitrary code execution in the host process.

The T2 driver adds one new attack surface vs. T1: the IR → machine-code
lowering emits bytes from `ir::NodeKind`-keyed emission tables. An
attacker who controls the IR (via a malformed RBC program that passes
the verifier but produces a hostile IR) could potentially steer the
emitted bytes into a useful gadget. The v0 mitigations:

- the IR verifier runs before every lowering (Rule 40 form; fail-closed
  on any verification failure),
- the lowering is fail-closed on any unhandled `NodeKind` (returns
  `nullptr` with a refusal reason; the engine falls back to T0),
- the W^X publish path is the same as T1's (`mprotect` flips; never
  writable-while-executable; `Stencil Rule 5` form).

The v1 hardening (execute-only memory, constant blinding, per-thread
PKRU) is documented in `docs/jit_hardening.md` and is open work.

---

## Change control

Changes to this contract go through the message system under team key
`codegen` (`docs/teams/messaging.md`). The v0 → v1 transition will be
announced with an INFO message and an accompanying ADVISORY to every
affected team (interpreter, baseline_noir, ir, passes, regalloc, aot,
gc/runtime) before the v1 code lands.

Until v1 lands, the T2 driver is opt-in via `b2t2` (the `b2jit` path
remains the default for T1 execution; `b2run` remains the default for
T0). The `b2graph --exec` mode is unchanged (it still wires the T2 path
with optimization on by default; the divergence on `strings_intern.rbc`
in that mode is the same `MSG-20260918-006` item 4 bug).

---

## See also

- `docs/STATUS.md` — open-work entry on the T2 driver (item 4)
- `docs/codegen_contract.md` — codegen team contract
- `docs/pass_contracts.md` — passes team contract (the optimization
  pipeline the driver optionally runs)
- `docs/jit_hardening.md` — JIT hardening v0 (W^X) and v1 (MPK,
  constant blinding, per-thread PKRU, constant-time, resource limits)
- `docs/deopt_backend.md` — T2/T3 deopt backend (design only; the v0
  operationally reuses T1 deopt)
- `compiler/codegen/tools/b2t2.cpp` — the T2 driver surface
- `compiler/codegen/src/T2Lowering.cpp` — the IR → x86-64 lowering
  (`lowerOnly`)
- `tests/codegen/T2CorpusTest.cpp` — the differential law test
  (Rule 36 form; 16/19 today; 3 known bugs in `MSG-20260918-006`)
- `messages/open/MSG-20260918-006-...BUG.md` — the known T2 divergences
