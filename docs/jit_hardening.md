# B-2 JIT Hardening — Design (v0)

Owner: Codegen Team (`codegen`; see `docs/teams/codegen-team.md`) +
Integrator (`.github/`, hardening policy)

```text
Normative reference: docs/laws.md
If this document conflicts with docs/laws.md, docs/laws.md wins.
```

**Status: v0 design — partial implementation.** W^X is honored (every
JIT buffer goes through `mprotect` flips: never writable-while-
executable, see `compiler/codegen/src/Instantiate.cpp` and
`compiler/codegen/src/T2Lowering.cpp`). All other hardening described
in this document is open work tracked in `docs/STATUS.md`.

This document exists to give adopters a single honest reference for
what the JIT does today to harden itself against misuse, what it does
not do, and what the v1 plan is. JIT spraying, execute-only memory,
and `PKEY_MPROTECT`/MPK isolation are not optional polish for a
production JIT — they are the difference between "RCE on any
attacker-controlled input" and "sandboxed compilation."

---

## What is implemented today

### W^X (write-XOR-execute)

Every executable buffer in the JIT goes through a strict W^X
discipline:

1. Allocate a page-aligned buffer (`posix_memalign` in
   `Instantiate.cpp`, `std::aligned_alloc` in `T2Lowering.cpp`).
2. Stage the bytes while the buffer is `PROT_READ | PROT_WRITE`
   (never executable at this stage).
3. Patch helper call sites with the buffer's absolute address (the
   helpers are external C functions in the host process).
4. Flip the buffer to `PROT_READ | PROT_EXEC` (never writable at this
   stage).
5. `__builtin___clear_cache` for the buffer's range (ARM/icache
   coherency; a no-op on x86-64 but harmless).

This satisfies Stencil Rule 5 (publish discipline). The window where
the buffer is RWX is zero — the buffer is never simultaneously
writable and executable.

### Heap-allocated buffer tracking

Each `CompiledCode` carries its `exec_alloc_size` so the teardown
path can `free` the right number of bytes; the buffer's lifetime is
bounded by the owning `CompiledCode` instance. There is no global JIT
arena that lives forever; per-method `CompiledCode` instances are
allocated and freed individually.

### Refusal on helper-address failure

`Instantiate.cpp` line 858-864: if a helper's address cannot be
resolved at publish time (the helper id is unknown), the buffer is
freed and the instantiation is refused with `BadHole`. No code is
published with an unresolved helper call.

---

## What is NOT implemented (open work)

### Execute-only memory

Today every JIT buffer is `PROT_READ | PROT_EXEC` after publish. On
x86-64, the CPU needs `PROT_READ` for the instruction fetch unit, so
execute-only is not directly possible at the hardware level for the
uniform page-table entry. The mitigations that *are* possible:

- **MPK (Memory Protection Keys)** on Intel CPUs (Skylake-SP and
  later, plus the recent `PKEY_EXEC_ONLY`-equivalent on AMD) allows a
  page to be marked execute-only at the PKRU register level. Use
  `pkey_alloc(0, PKEY_EXEC_ONLY)` + `pkey_mprotect(...)` to mark the
  JIT buffer execute-only. PKRU is per-thread, so this is a JIT-thread
  isolation boundary, not a global one.
- **`memfd_create` + `mmap(MAP_PRIVATE | MAP_EXEC)`** instead of
  `posix_memalign` + `mprotect`. The buffer is then not backed by
  `/dev/zero` or any heap arena; it lives in its own memory mapping
  and is reclaimable via `munmap`. This is a small hardening (no
  metadata leak from malloc internals) and a small win on teardown
  (address space returns to the OS, not to a heap arena).
- **`MAP_POPULATE`** to avoid page-fault latency on first execution
  (a latency hardening, not a security one).

None of these are implemented today. The v1 plan is to add a
`B2_JIT_HARDEN` CMake option (default ON in Release, OFF in Debug)
that switches allocation from `posix_memalign` to
`mmap(MAP_PRIVATE | MAP_ANONYMOUS)` + `PROT_NONE` initial state, then
`PROT_READ | PROT_WRITE` for staging, then `PROT_READ | PROT_EXEC`
for publish, and optionally MPK on top.

### JIT spraying mitigations

JIT spraying is the attack where the attacker controls a constant
embedded in the JIT's machine code (e.g. an immediate value, a
constant-pool entry) and constructs a sequence of immediate bytes
that, when jumped into mid-instruction, decode as a useful gadget.

The B-2 JIT has several JIT-spraying surfaces today:

- `Instantiate.cpp` patches helper call sites with an absolute
  8-byte address (line 857-870). The address is the runtime address
  of a C function; an attacker who controls the helper id (via a
  malformed RBC method, see `tests/rbc/corpus/bad_*.rbc`) could
  potentially steer the address into a useful gadget. The verifier's
  refusal catalog (`docs/rbc_spec.md`) lists the malformed-method
  refusals; the verifier is the hard gate.
- `StencilBodies.cpp` embeds raw stencil bytes (the stencil archive
  is itself embedded into the `b2jit` binary by `Embedded.cpp`). An
  attacker who controls the archive pwns the process. The archive
  is currently build-time-only and not loadable from disk; this is a
  mitigation-by-architecture, not by code.
- The RBC constant pool (`include/b2/rbc/Rbc.h` `Const::Kind::Int32`
  etc.) carries attacker-controlled 32-bit and 64-bit constants that
  the JIT lowers as immediate operands in `mov` instructions. An
  attacker who can construct a constant with specific bytes has
  control over a slice of the JIT's output. The verifier's type
  check ensures the constant's kind matches the instruction's
  expected kind, but does not constrain the constant's *value*.

The v1 plan is:

- **Constant blinding**: for every attacker-controlled constant,
  XOR the constant with a per-publish random 32-bit mask, emit the
  XORed value as the immediate, and emit an `XOR r, mask` immediately
  before the use. This makes the JIT's output not byte-controllable
  by the attacker.
- **Constant pooling**: move attacker-controlled constants to a
  dedicated read-only data section (not inline in the code buffer),
  so they cannot be jumped into mid-instruction.
- **NOP insertion**: pad between stencils with random-length NOP
  sequences so the attacker cannot predict the address of a specific
  gadget.

### Per-thread PKRU isolation

`PKRU` allows a per-thread execute-only page. Today the JIT's
allocation lives in the host process's address space, accessible from
any thread that knows the address. The v1 plan is:

- `pkey_alloc(0, PKEY_EXEC_ONLY)` at JIT initialization.
- `pkey_mprotect(exec_buf, ..., pkey)` on every JIT buffer publish.
- `wrpkru` to set the executing thread's PKRU to allow execution on
  that key; other threads' PKRU does not allow it, so the JIT
  buffer is execute-only from the mutator's perspective.

This is the strongest available JIT hardening on Intel/AMD; it is
gated on CPUID detection at process start (not all x86-64 has MPK).
Today there is no MPK code in the tree.

### Constant-time blinding

For constant-time code paths (e.g. cryptographic comparisons inlined
into the JIT) the JIT must not branch on secret values. The v1 plan
is:

- a `CompileOptions::Flag::ConstantTime` flag that, when set, forces
  the codegen to emit constant-time instruction sequences for branches
  (`cmov` instead of `jcc`), refuse to lower any instruction that
  cannot be made constant-time, and emit a `NOP` fence around the
  sensitive region.

Today `CompileOptions` has no such flag; this is open work.

### Resource limits (per-method compile budget)

`docs/laws.md` and `docs/baseline_contract.md` describe a per-method
compile budget (the kill switch) via `PlanBudget`. Today `PlanBudget`
exists and is exercised by the baseline (T1) plan builder. The T2
path does not honor a budget because the T2 driver does not exist.
Resource limits on the JIT heap size, stack depth, and bytecode size
are not implemented today; the v1 plan is to add them as part of the
async compilation work (`docs/STATUS.md` item 5).

---

## Threat model

The B-2 JIT's threat model assumes:

- The RBC program text is attacker-controlled (a malicious user can
  craft an `.rbc` file).
- The RBC verifier is the hard gate; any program that passes the
  verifier is assumed to be well-formed enough to not crash the
  interpreter, but not necessarily benign (the verifier checks types
  and structural well-formedness, not behavior).
- The host process's heap, stack, and code segments are not directly
  attacker-writable; the attacker's only writable surface is the
  attacker-supplied RBC text and constant pool.
- The attacker's goal is arbitrary code execution in the host
  process (RCE), and the attacker will use any constant-byte control
  over the JIT's output to construct gadgets.

Given this threat model, the v0 hardening (W^X + helper-address
refusal) is necessary but not sufficient. The v1 hardening
(execute-only memory + constant blinding + per-thread PKRU +
resource limits) is the minimum bar for "safe to embed in a
security-sensitive host."

---

## See also

- `docs/STATUS.md` — open-work entry on JIT hardening
- `docs/codegen_contract.md` — codegen team contract
- `docs/baseline_contract.md` — baseline (T1) contract (PlanBudget,
  the kill switch)
- `docs/stencils.md` — stencil format contract (W^X is Stencil
  Rule 5)
- `docs/teams/codegen-team.md` — codegen team charter
- `compiler/codegen/src/Instantiate.cpp` — T1 instantiation
  (the W^X publish path lives here, line 843-878)
- `compiler/codegen/src/T2Lowering.cpp` — T2 lowering (the W^X
  publish path lives here, line 1239-1252)
