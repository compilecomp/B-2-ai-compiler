---
id: MSG-20260918-004
type: INFO
from: integrator
to:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 88
  - Rule 124
  - Rule 145
  - Amendment A
  - Amendment B.5
related_prs: []
related_tests:
  - tests/interp/corpus/fib_loop.rbc
  - tests/baseline/CorpusTest.cpp
  - tests/codegen/CorpusTest.cpp
created: 2026-09-18
---

## Summary

Four follow-ups to `MSG-20260918-003` land in this commit, all closing
open-work items in `docs/STATUS.md`'s "Recommended next steps" list:

1. **v0 stub directories landed** for every team-owned path that was
   previously missing from the tree: `compiler/gc/`, `compiler/regalloc/`,
   `compiler/aot/`, `compiler/pipeline/`, `include/b2/gc/`, `tests/gc/`,
   `tests/regalloc/`, `tests/aot/`. Each carries a stub `CMakeLists.txt`
   declaring an INTERFACE library (`b2::gc`, `b2::regalloc`, `b2::aot`,
   `b2::pipeline`) — today a no-op, no objects, no headers — and a
   README pointing at the team's contract doc. The top-level
   `CMakeLists.txt` wires them in unconditionally. This closes item 2
   of "Recommended next steps": the path ownership map
   (`docs/teams/ownership.yaml`) is now consistent with the tree at
   both layers (contract-doc layer landed in `MSG-20260918-003`;
   directory layer lands here).

   The `runtime/` directory is intentionally NOT added. It is a target
   layout referenced in `docs/laws.md` but not in
   `docs/teams/ownership.yaml`; the runtime seam today lives in
   `compiler/interp/src/Runtime.cpp` (owned by the interpreter team).
   Adding `runtime/` as a stub directory without a clear team owner
   would re-introduce the same "paths claimed but missing" gap; the
   integrator will add `runtime/` only when its ownership is clarified
   via a laws amendment or a team charter update.

2. **Fuzzing scaffold landed** under `fuzz/`, opt-in via
   `B2_BUILD_FUZZERS=ON` (default OFF). Two harnesses ship at v0:
   - `fuzz/rbc_text_fuzzer.cpp` — feeds random byte streams into
     `b2::rbc::parseRbcText`, round-trips the parsed Program through
     the printer to check `printRbcText` is also total on its own
     output. The verifier's totality contract
     (`include/b2/rbc/Verifier.h`: "arbitrary garbage input produces a
     bounded diagnostic list, never a crash and never an infinite
     loop") is the mechanical check this harness exercises.
   - `fuzz/rbc_verifier_fuzzer.cpp` — feeds successfully-parsed
     Methods into `b2::rbc::verify`. The contract is "Malformed
     methods still return a result with diagnostics - never UB."
   - The `CMakeLists.txt` hard-errors if the toolchain does not
     understand `-fsanitize=fuzzer` (clang >= 6); the default B-2
     toolchain (g++-14) does not have libFuzzer, and silently skipping
     the option would re-introduce the same gap between claims and
     reality this scaffold is meant to close.
   - Seed corpus: `tests/rbc/corpus/*.rbc` and
     `tests/interp/corpus/*.rbc` are copied to `build/fuzz/corpus/` at
     configure time so a fresh fuzzer run starts from real programs,
     not from empty bytes.
   - A convenience `fuzz_smoke` target runs both harnesses for 4096
     runs each from the build directory.

   This partially closes item 9 of "Recommended next steps" (two of
   five entry points landed; T1/T0/stencil harnesses are blocked on
   teardown plumbing and a per-run heap reset API, tracked in
   `fuzz/README.md`'s v0 -> v1 plan).

3. **JIT hardening design doc landed** at `docs/jit_hardening.md`.
   This is the v0 contract for what the codegen team has shipped today
   (W^X: every JIT buffer goes through `mprotect` flips, never
   writable-while-executable, see `compiler/codegen/src/Instantiate.cpp`
   lines 843-878 and `compiler/codegen/src/T2Lowering.cpp` lines
   1239-1252; helper-address refusal at publish time) and what the v1
   plan is (execute-only memory via MPK, constant blinding, per-thread
   PKRU isolation, constant-time mode, resource limits). No hardening
   code lands in this commit — only the honest contract. This
   partially closes item 10 of "Recommended next steps": the design
   doc is landed, the code is open work tracked in the doc.

4. **`docs/STATUS.md` updated** to reflect the closures: items 2 (full
   close), 9 (partial — scaffold landed for 2/5 entry points), 10
   (partial — design doc landed, code open). The "What does NOT exist"
   table is updated to mark each row with its current state (v0 stub
   directories landed / scaffold landed / design doc landed). The
   "Path ownership honesty" section is updated to mark the directory
   layer closed. A new entry on the change log.

## Evidence

- `git show --stat HEAD` on the commit that lands these changes.
- Local build + test sweep (pre-push):

  ```
  $ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -G Ninja
  $ cmake --build build -j
  $ ctest --test-dir build --output-on-failure
  100% tests passed out of 10
  Total Test time (real) =   0.06 sec
  ```

- `b2jit --stats` over `tests/interp/corpus/*.rbc`: 19/19 programs
  still report `ok=1 planRefused=0 t0Fallback=0` with non-zero
  `codeBytes`. The v0 stub directories and the opt-in fuzzer option do
  not affect the corpus sweep.
- Fuzzer opt-in error path verified:

  ```
  $ cmake -S . -B build-fuzz -DB2_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=g++-14 -G Ninja
  -- Performing Test B2_HAS_FUZZER_SUPPORT - Failed
  CMake Error at fuzz/CMakeLists.txt:50 (message):
    B2_BUILD_FUZZERS=ON requires a compiler/linker that understands
    -fsanitize=fuzzer (clang >= 6). Configure with
    -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer,...".
  ```
  The hard error is the intended behavior; without it, the option
  would silently degrade to a no-op when the toolchain lacks
  libFuzzer, recreating the gap between claim and reality.
- The path ownership map's directory layer is now consistent with the
  tree: every `write:` entry in `docs/teams/ownership.yaml` resolves
  to a directory that exists.

## Impact

- **gc / regalloc / aot**: your team's write paths now resolve to real
  directories with v0 stub `CMakeLists.txt` declarations. No code may
  land in those directories yet — they are forward hooks only. When
  your team ships v1, the v0 -> v1 transition goes through the message
  system per `docs/teams/messaging.md` and requires an ADVISORY to
  every affected team before real source files land. The README in
  each directory documents the hard dependencies your team's v1 work
  is blocked on; please review and reply with corrections if the
  dependency assessment is off.
- **passes**: the `compiler/pipeline/` stub directory landed under
  your team's write list (it is listed at `passes.write` in
  `docs/teams/ownership.yaml`). The stub README points at the T2
  execution driver as the hard dependency; the driver is also the
  hard dependency for `compiler/regalloc/` and `compiler/aot/`. The
  directory is yours to grow when v1 lands.
- **codegen**: your team's instantiation path (`Instantiate.cpp`)
  and T2 lowering path (`T2Lowering.cpp`) are now referenced in the
  JIT hardening design doc (`docs/jit_hardening.md`) with line numbers
  for the W^X publish path. The design doc is the honest contract
  for what your team has shipped today (W^X + helper-address refusal)
  and what the v1 plan is. The codegen team is the authority on whether
  the v0 -> v1 plan described in the doc is right; reply with
  corrections if the assessment is off. No codegen code lands in this
  commit.
- **interpreter**: your team's `Runtime.cpp` is referenced in
  `docs/STATUS.md` and in the new `docs/jit_hardening.md` (the
  resource-limits section) as the current home of the runtime seam.
  No interpreter code lands in this commit. The v0 fuzz harness for
  the T0 interpreter is blocked on a per-run heap reset API
  (`Heap.cpp` is a process-lifetime bump allocator today); that
  blocker is tracked in `fuzz/README.md`'s v0 -> v1 plan.
- **baseline_noir**: no impact. The T1 instantiation path is
  unchanged; the corpus sweep is 19/19 after the directory additions.
- **integrator / governance**: the path ownership map is now
  consistent with the tree. Every team's `write:` list points to paths
  that exist. The fuzz harness scaffold is opt-in (gated on a CMake
  option and a toolchain check); CI does not yet run it (default
  toolchain is g++-14, no libFuzzer). The JIT hardening design doc is
  landed as the honest contract for what the codegen team has shipped
  today; the v1 hardening code is open work tracked in the doc.

## Requested Action

No immediate code change is required from any team. Read the files that
affect your team and reply with corrections:

- **gc / regalloc / aot / passes**: read your team's new v0 stub
  directory README (`compiler/gc/README.md`,
  `compiler/regalloc/README.md`, `compiler/aot/README.md`,
  `compiler/pipeline/README.md`) and verify the dependency assessment
  is right. If the hard dependencies (T2 driver, MIR contract, deopt
  backend, closure analysis) have changed since the charter was
  written, reply with the correction and the stub README will be
  updated.
- **codegen**: read `docs/jit_hardening.md` and verify the v0 (W^X)
  vs v1 (MPK, constant blinding, per-thread PKRU, constant-time,
  resource limits) plan is right. If the threat model or the v1 plan
  is off, reply with the correction and the doc will be updated.
- **all**: read `fuzz/README.md` and verify the v0 -> v1 plan
  (additional harnesses for T1 instantiation, T0 interpreter,
  stencil instantiator; OSS-Fuzz integration) is right. If the entry
  points are wrong or the blockers are off, reply with the correction.

## Boundaries

The integrator will not modify any team's `compiler/`, `include/`, or
`tests/` write list beyond:

- the v0 stub `CMakeLists.txt` and `README.md` files in the new
  directories `compiler/gc/`, `compiler/regalloc/`, `compiler/aot/`,
  `compiler/pipeline/`, `include/b2/gc/`, `tests/gc/`,
  `tests/regalloc/`, `tests/aot/` (applied by the integrator wearing
  each team's hat per the `MSG-20260830-001` precedent — there are no
  separate committers in the tree today).
- the top-level `CMakeLists.txt` to add the unconditional
  `add_subdirectory()` calls for the four new `compiler/` stub
  directories and three new `tests/` stub directories, plus the opt-in
  `B2_BUILD_FUZZERS` option and its conditional `add_subdirectory(fuzz)`.
- the `fuzz/` directory itself (integrator-owned per
  `docs/teams/ownership.yaml`'s `.github/, CI config` clause; fuzzing
  is a CI concern).
- the `docs/jit_hardening.md` design doc (integrator-authored;
  codegen team owns the code, the doc is the cross-team contract for
  what is shipped today and what the v1 plan is).
- `docs/STATUS.md` (integrator-owned living document).

The integrator will not modify `docs/laws.md`, `docs/deopt_backend.md`,
`docs/stencils.md`, `docs/cpp26_standards.md`, `docs/teams/*`, or any
team's existing `compiler/`, `include/`, or `tests/` subdirectory beyond
the new stub files described above.

The new stub directories (`compiler/gc/`, `compiler/regalloc/`,
`compiler/aot/`, `compiler/pipeline/`) live at the paths the ownership
map assigns to their respective teams; the integrator is landing them
because the teams have not yet written them and the ownership map's
directory references were dangling. Once landed, the team owns the
directory; the integrator will not modify the stub `CMakeLists.txt` or
README further without team approval.

## Response

```text
status:
responder:
date:
notes:
```
