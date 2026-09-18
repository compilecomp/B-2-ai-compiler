# `fuzz/` — libFuzzer harnesses (v0 scaffold)

**Status:** v0 scaffold. **Two harnesses landed**: RBC text parser, RBC
verifier. **Opt-in build** (off by default; requires clang +
`-fsanitize=fuzzer`).

This directory is the B-2 project's fuzzing scaffold. It exists because
the JIT emits executable machine code and the RBC verifier is the hard
gate before any tier executes RBC (`docs/laws.md`: "verifier before
quickener before execution"). Fuzzing is the cheapest way to find the
gaps between the contract ("arbitrary garbage input produces a bounded
diagnostic list, never a crash and never an infinite loop",
`include/b2/rbc/Verifier.h`) and the implementation.

The CI workflow (`.github/workflows/ci.yml`) does NOT run the fuzzers
today; they are opt-in via `B2_BUILD_FUZZERS=ON`. This is honest: the
default toolchain (g++-14) does not have libFuzzer, and silently
skipping the option would create the same gap between claims and
reality that this scaffold is meant to close. Future work tracked in
`docs/STATUS.md` includes wiring this into OSS-Fuzz once the harness
set is broad enough.

## What lands here at v0

- `rbc_text_fuzzer.cpp` — feeds random byte streams into
  `b2::rbc::parseRbcText`. Round-trips the parsed Program through the
  printer to check `printRbcText` is also total on its own output.
- `rbc_verifier_fuzzer.cpp` — feeds successfully-parsed Methods into
  `b2::rbc::verify`. The contract is "arbitrary Methods produce a
  bounded diagnostic list, never UB."
- `CMakeLists.txt` — opt-in build under `B2_BUILD_FUZZERS=ON`. Hard
  configure error if the toolchain does not understand
  `-fsanitize=fuzzer`. Seeds both harnesses from the existing
  `tests/rbc/corpus/*.rbc` fixtures (maximizes early coverage).
- A convenience `fuzz_smoke` target runs both harnesses for 4096 runs
  each from the build directory.

## Build

```sh
cmake -S . -B build-fuzz \
  -DB2_BUILD_FUZZERS=ON \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all"
cmake --build build-fuzz
./build-fuzz/fuzz/rbc_text_fuzzer -max_total_len=65536 -max_len=4096
./build-fuzz/fuzz/rbc_verifier_fuzzer -max_total_len=65536 -max_len=4096
# or the convenience target:
cmake --build build-fuzz --target fuzz_smoke
```

## What lands here at v1

When the fuzzing scaffold graduates to v1, this directory will hold:

- an RBC corpus seed directory under `fuzz/corpus/` (currently seeded
  at build time from `tests/rbc/corpus/` — moving to a tracked tree
  would let the corpus survive across builds),
- a T1 instantiation harness (the `compiler/codegen/` Instantiate.cpp
  path) — requires plumbing `CompiledCode`'s teardown to the fuzzer's
  exit so the JIT arena doesn't leak across runs,
- a T0 interpreter harness (the `compiler/interp/` Interp.cpp path) —
  requires a per-run heap reset (today's `Heap.cpp` is a process-lifetime
  bump allocator with no reset API),
- a stencil instantiator harness for `tools/stencilgen/`,
- a coverage report target (`llvm-profdata` + `llvm-cov` integration),
- OSS-Fuzz integration (`project.yaml` + a build script under
  `fuzz/oss-fuzz/`).

The v0 -> v1 transition goes through the message system per
`docs/teams/messaging.md`; the harness set is integrator-owned per
`docs/teams/ownership.yaml` (the `.github/` clause covers fuzzing as a
CI concern).

## Non-guarantees (v0)

- The fuzzers are not run by default; CI does not exercise them.
- No coverage report is generated.
- No T1 / T0 / stencil harness exists yet (the JIT arena and the heap
  are not fuzzing-safe today).
- No OSS-Fuzz integration.

## See also

- `docs/STATUS.md` — open-work entry on fuzzing
- `docs/rbc_spec.md` — the text format the parser consumes
- `include/b2/rbc/Verifier.h` — the verifier's totality contract
- `tests/rbc/` — the seed corpus
- `.github/workflows/ci.yml` — CI does not yet run fuzzers (acknowledged
  in `docs/STATUS.md`)
