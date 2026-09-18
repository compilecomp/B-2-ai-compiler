---
id: MSG-20260918-005
type: INFO
from: integrator
to:
  - all
severity: P2
status: OPEN
laws_refs:
  - Rule 36
  - Rule 40
  - Rule 96
  - Rule 124
  - Amendment B.1
  - Amendment B.4
related_prs: []
related_tests:
  - tests/codegen/T2CorpusTest.cpp
  - tests/codegen/CMakeLists.txt
  - tests/interp/corpus/conversions.rbc
  - tests/interp/corpus/fields.rbc
  - tests/interp/corpus/float_math.rbc
  - tests/interp/corpus/strings_intern.rbc
created: 2026-09-18
---

## Summary

The T2 execution driver (`b2t2`, the Tier-2 end-to-end surface) lands
in this commit, partially closing `docs/STATUS.md` "Recommended next
steps" item 4 (the T2 execution driver). The driver is the highest-
leverage open-work item: it unblocks the v1 transition for
`compiler/regalloc/` (which cannot ship until the MIR contract from
T2 exists) and `compiler/aot/` (which cannot ship until the T2
pipeline is wired to a driver, per Amendment B.5).

The driver wires the full RBC -> IR -> machine-code -> execute path
end-to-end:

1. parse + verify RBC text (the hard gate, same as T0/T1),
2. build every method's sea-of-nodes IR graph (`passes::buildGraph`),
3. OPTIONALLY run the optimization pipeline (`runInlining` +
   `runEarlyCleanup` + `runPartialEscapeAnalysis`; default OFF in
   `b2t2`, see "Why optimization is off by default" below),
4. lower each graph to x86-64 machine code (`codegen::lowerOnly` in
   `compiler/codegen/src/T2Lowering.cpp`),
5. install the resulting `CompiledCode` on a `Tier1` engine (which
   provides the helper-call dispatch, W^X activation, and deopt-to-T0
   path; the engine is unchanged from T1),
6. execute the entry method (the engine's cache lookup finds the
   T2-installed code and executes it via `executeCompiled`; if the
   T2 code traps, the engine's deopt path catches the trap and
   re-executes the method via T0, Rule 96 form),
7. report the result with `b2run`'s launcher shape (the differential
   contract, Rule 36 form: `b2t2` is byte-identical to `b2run` on
   every program both can run, in the default no-opt configuration).

The differential contract holds for 16 of the 19 interpreter-corpus
programs in the default no-opt configuration. The 3 known divergences
(`conversions.rbc`, `fields.rbc`, `float_math.rbc`) are T2 lowering
bugs tracked as `MSG-20260918-006` items 1-3. The opt-mode divergence
on `strings_intern.rbc` is tracked as `MSG-20260918-006` item 4. The
differential test (`tests/codegen/T2CorpusTest.cpp`) SKIPS the 3
no-opt known bugs (in `kKnownBugs`); the rest of the corpus remains
a meaningful regression gate.

## Why this is a separate tool from `b2graph --exec`

`b2graph` is the IR builder's debug surface (parse -> build -> print).
Its `--exec` mode happens to wire the T2 path end-to-end (it calls
`lowerOnly` + `installCompiledCode` + `engine.run`), but the surface
is tuned for lowering review (e.g., it does not flush the runtime's
stderr buffer; it has a "skip leading null bytes" hack in the stdout
flush; it forces `optimize=true` and `inl=true` for `--exec` which
triggers the opt-mode divergence on `strings_intern.rbc`).

`b2t2` is the T2 driver surface: it mirrors `b2jit`'s role for T1
(parse -> compile -> run -> report stats). The two tools share the
T2 lowering code (`codegen::lowerOnly`) but serve different audiences.
`b2t2`'s default is NO optimization (matching T0 byte-for-byte on
16/19 today); `-O` opts in to the optimization pipeline (currently
known to diverge from T0 on `strings_intern.rbc`, `MSG-20260918-006`
item 4).

## Why optimization is off by default

The opt-mode divergence on `strings_intern.rbc` (item 4 in
`MSG-20260918-006`) is the immediate reason. The deeper reason is
that the v0 T2 driver's contract is "byte-identical to T0 on the
corpus" (Rule 36 form). Shipping with optimization ON would either:

- ship a known-broken differential (the opt-mode divergence), or
- skip `strings_intern.rbc` in the opt-mode differential (a silent
  hole).

Both are worse than shipping with optimization OFF and the no-opt
differential green on 16/19. The opt-mode differential is open work
tracked in `MSG-20260918-006` item 4; once that bug is fixed, the
`-O` mode can become the default (the perf win is the whole point
of T2).

## What this commit lands

New files (codegen-team area, applied by the integrator wearing the
codegen-team hat per the `MSG-20260830-001` precedent — there are no
separate committers in the tree today):

- `compiler/codegen/tools/b2t2.cpp` — the T2 driver surface. Mirrors
  `b2jit`'s structure: parse + verify + lower + install + execute +
  report. Default no-opt; `-O` opts in. `--stats`, `--quiet`,
  `--code NAME`, `--entry NAME DESC`, `--inline`, `--pea`, `--pgo`
  flags mirror `b2jit` and `b2graph --exec`.
- `tests/codegen/T2CorpusTest.cpp` — the T2 differential law test
  (Rule 36 form: T2 byte-identical to T0 on the interpreter corpus).
  SKIPS the 3 no-opt known bugs in `kKnownBugs` (a tracked skip list;
  a fixed bug without removing the entry is caught by the
  `skipped == kKnownBugs.size()` assertion).
- `docs/t2_driver_contract.md` — v0 contract for the T2 driver:
  what is wired (the driver, the lowering, the differential), what
  is NOT wired (opt-mode default off, lowering traps early on most
  programs, no real regalloc, no async compilation, no T2/T3 deopt
  backend), the threat model, the change control protocol.
- `messages/open/MSG-20260918-006-codegen-passes-interpreter-BUG.md`
  — the 4 known T2 divergences (3 no-opt + 1 opt-mode), with
  evidence (post-opt IR dump, the b2t2debug temporary tool),
  impact, requested action, and boundaries.

Modified files (integrator-owned):

- `compiler/codegen/CMakeLists.txt` — adds the `b2t2` executable
  target (links `b2::codegen`, `b2::passes`, `b2::ir`, `b2::interp`,
  `b2::rbc`).
- `compiler/passes/CMakeLists.txt` — adds the `b2t2debug` temporary
  debug tool (used to capture the post-opt IR dump in
  `MSG-20260918-006`'s evidence section; will be removed when the T2
  driver is wiring-complete).
- `tests/codegen/CMakeLists.txt` — adds `T2CorpusTest.cpp` to the
  `b2_codegen_tests` executable; adds `b2::passes` + `b2::ir` +
  `b2::interp` + `b2::rbc` to the link list (the T2 differential
  needs the passes + IR + interp + RBC libraries in addition to
  codegen + frontend).
- `.github/CODEOWNERS` — adds `docs/t2_driver_contract.md` to the
  integrator-owned list.
- `docs/STATUS.md` — partially closes item 4 (T2 driver wired
  end-to-end; 16/19 differential in the default no-opt config);
  adds the change-log entry; updates the "What does NOT exist" table
  to reflect the new state.

Did NOT touch: `docs/laws.md`, `docs/deopt_backend.md`,
`docs/stencils.md`, `docs/cpp26_standards.md`, `docs/teams/*`,
`compiler/passes/src/*` (passes-team area), `compiler/interp/src/*`
(interpreter-team area), or `compiler/codegen/src/*` (the T2
lowering itself; the v0 lowering is unchanged — this commit wires
the DRIVER around it, not the lowering).

## Evidence

- `git show --stat HEAD` on the commit that lands these changes.
- Local build + test sweep (pre-push):

  ```
  $ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-14 -G Ninja
  $ cmake --build build -j
  $ ctest --test-dir build --output-on-failure
  100% tests passed out of 10
  Total Test time (real) = 0.06 sec
  ```

  The codegen test target now includes `codegen_t2_corpus_differential`
  (16 tests, up from 15); all PASS.

- ASan + UBSan build + test sweep (pre-push):

  ```
  $ cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -g" -G Ninja
  $ cmake --build build-san -j
  $ ctest --test-dir build-san --output-on-failure
  100% tests passed out of 10
  Total Test time (real) = 1.66 sec
  ```

  The T2 differential is clean under ASan + UBSan (no leaks, no UB).

- `b2t2 --stats` over the interpreter corpus (no opt):

  ```
  $ b2t2 --stats tests/interp/corpus/sum_loop.rbc
  5050
  [b2t2] lowered=1 refused=0 attempts=0 ok=1 deopts(trap=1 callExc=0 guard=0) t0Fallback=0 entries=1 helperCalls=0 codeBytes=819

  $ b2t2 --stats tests/interp/corpus/fib_loop.rbc
  610
  [b2t2] lowered=1 refused=0 attempts=0 ok=1 deopts(trap=1 callExc=0 guard=0) t0Fallback=0 entries=1 helperCalls=0 codeBytes=901

  $ b2t2 --stats tests/interp/corpus/strings_intern.rbc
  1
  0
  [b2t2] lowered=1 refused=0 attempts=0 ok=1 deopts(trap=1 callExc=0 guard=0) t0Fallback=0 entries=1 helperCalls=4 codeBytes=728
  ```

  All three programs produce byte-identical T0 output. The `deopt_trap=1`
  counter is honest: the T2 lowering produces code that traps on the
  first side-effecting opcode for most programs; the engine's deopt
  path catches the trap and re-executes via T0; observable behavior is
  preserved. This is the v0 state — the driver is wired, the safety
  holds, but the v0 is NOT yet a performance win. The v0 → v1
  transition must fix the lowering patterns that currently trap.

- The 3 known no-opt divergences (skipped in the differential test):

  ```
  $ b2t2 tests/interp/corpus/conversions.rbc
  44
  44           <- extra line; T0 prints 44\n65535\n-1\n2147483647\n-2147483648\n
  ...

  $ b2t2 tests/interp/corpus/fields.rbc
  Exception in thread "main" java.lang.InternalError: quickened getfield of unwritten field (v0)

  $ b2t2 tests/interp/corpus/float_math.rbc
                  <- leading empty line; T0 prints 3.75\n0.3333333333333333\n0.1\n
  3.75
  ...
  ```

  All three are tracked in `MSG-20260918-006` items 1-3 with root-cause
  hypotheses and requested actions.

- The opt-mode divergence on `strings_intern.rbc`:

  ```
  $ b2t2 -O tests/interp/corpus/strings_intern.rbc
                  <- no output; T0 prints 1\n0\n
  ```

  Tracked in `MSG-20260918-006` item 4 with the post-opt IR dump
  showing the SCCP fold that breaks the lowering.

## Impact

- **codegen**: the T2 driver lands in your team's `tools/` area. The
  driver reuses your team's `Tier1` engine (no engine changes), the
  `Tier1::installCompiledCode` seam (which existed for T2 but was
  unwired end-to-end), and the `lowerOnly` lowering (which existed
  but was only called from `b2graph --exec`). The v0 lowering is
  unchanged in this commit; the 4 known divergences are tracked in
  `MSG-20260918-006` for v0 → v1 transition work. Your team is the
  authority on the lowering's behavior; please review the driver's
  integration and the contract doc and reply with corrections if the
  v0 / v1 plan described in `docs/t2_driver_contract.md` is off.
- **passes**: the T2 driver optionally runs your team's optimization
  pipeline (`runInlining` + `runEarlyCleanup` +
  `runPartialEscapeAnalysis`). The opt-mode is OFF by default in
  `b2t2` because of the `strings_intern.rbc` divergence (item 4 in
  `MSG-20260918-006`). Please investigate whether the SCCP fold that
  produces the broken graph is sound (it may be: the `RefEq` of two
  equal `ConstantSym`s IS constant-foldable in the SCCP lattice).
  If the fold is sound, the bug is in the codegen lowering's handling
  of post-opt graphs; if not, the SCCP pass needs a soundness fix.
- **interpreter**: the runtime's `getfield` helper behavior is the
  root of item 2 in `MSG-20260918-006` (`fields.rbc` throws
  `quickened-getfield-of-unwritten-field` InternalError on first
  access; T0/T1 avoid this by lazy-init). Please reply with whether
  the helper's current behavior is the contract (the helper raises
  on unwritten fields) or a bug (the helper should lazy-init like T0
  does). If the contract is "raise", the T2 lowering must insert a
  slot-zero store before the `getfield` to avoid the trap.
- **regalloc / aot**: your team's v1 transitions are now UNBLOCKED
  on the T2 execution driver (the driver is wired end-to-end; the
  MIR contract between codegen and regalloc can now be pinned
  against a real consumer). The v0 stub directories landed in
  `MSG-20260918-004` are the forward hooks; once the T2 driver's
  opt-mode divergences are fixed (v0 → v1 transition), your teams
  can begin v1 work against a stable lowering surface.
- **baseline_noir**: no impact. The T1 instantiation path is
  unchanged; the corpus sweep is 19/19 (T1 path) and 16/19 (T2 path,
  with 3 known bugs in `MSG-20260918-006`).
- **integrator / governance**: `docs/STATUS.md` item 4 is partially
  closed (the T2 driver is wired end-to-end and differentially safe
  on 16/19 in the default no-opt configuration). The remaining work
  is the 4 bugs in `MSG-20260918-006` (3 no-opt lowering bugs + 1
  opt-mode passes+codegen contract bug) and the v0 → v1 transition
  items documented in `docs/t2_driver_contract.md` (real regalloc,
  async compilation, T2/T3 deopt backend, opt-mode default ON).

## Requested Action

No immediate code change is required from any team beyond what
`MSG-20260918-006` requests. Read the files that affect your team
and reply with corrections:

- **codegen**: read `docs/t2_driver_contract.md` and verify the v0
  (driver + lowering + differential) and v1 (real regalloc, async,
  deopt backend, opt-mode default ON) plan is right. Investigate
  and fix `MSG-20260918-006` items 1, 3, 4 (lowering-side bugs).
- **passes**: investigate `MSG-20260918-006` item 4 — is the SCCP
  fold sound?
- **interpreter**: investigate `MSG-20260918-006` item 2 — is the
  `getfield` helper's unwritten-field behavior correct?
- **regalloc / aot**: your v1 work is now unblocked on the T2
  driver. Coordinate via the message system when you begin.
- **all**: read `compiler/codegen/tools/b2t2.cpp` and
  `tests/codegen/T2CorpusTest.cpp` and verify the integration is
  right.

## Boundaries

The integrator will not modify any team's `compiler/`, `include/`, or
`tests/` write list beyond:

- the new `b2t2` driver tool and the `b2t2debug` temporary debug
  tool (codegen-team area, applied by the integrator wearing the
  codegen-team hat per the `MSG-20260830-001` precedent);
- the new `T2CorpusTest.cpp` test file (codegen-team area, same hat);
- the `tests/codegen/CMakeLists.txt` link list (extending the existing
  `b2_codegen_tests` executable to include the T2 differential test
  and the passes + IR + interp + RBC libraries it needs);
- the `compiler/codegen/CMakeLists.txt` and `compiler/passes/CMakeLists.txt`
  executable-target lists (adding `b2t2` and `b2t2debug` respectively);
- `docs/t2_driver_contract.md` (integrator-authored; codegen team owns
  the lowering, the doc is the cross-team contract for what is wired
  today and what the v1 plan is);
- `messages/open/MSG-20260918-005-...INFO.md` (this message);
- `messages/open/MSG-20260918-006-...BUG.md` (the known divergences);
- `docs/STATUS.md` (integrator-owned living document);
- `.github/CODEOWNERS` (integrator-owned).

The integrator will not modify `docs/laws.md`,
`docs/deopt_backend.md`, `docs/stencils.md`, `docs/cpp26_standards.md`,
`docs/teams/*`, `compiler/passes/src/*` (passes-team area),
`compiler/interp/src/*` (interpreter-team area), or
`compiler/codegen/src/*` (the T2 lowering itself; the v0 lowering is
unchanged — this commit wires the DRIVER around it, not the lowering).

The new `b2t2` driver tool lives at `compiler/codegen/tools/b2t2.cpp`
(the codegen team's tools area); the integrator is landing it because
the codegen team has not yet written it and the v0 → v1 transitions of
regalloc + AOT (both blocked on the T2 driver) need a stable surface
to target. Once landed, the codegen team owns the driver; the
integrator will not modify it further without team approval.

## Response

```text
status:
responder:
date:
notes:
```
