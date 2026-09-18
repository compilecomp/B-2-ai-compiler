# B-2 Fuzz harnesses: v0 scaffold.
#
# WHY THIS IS OPT-IN:
#   Fuzz harnesses link libFuzzer (clang-only, requires
#   -fsanitize=fuzzer) or another fuzzing engine. The default B-2 build
#   is g++-14 + cmake + ninja (see .github/workflows/ci.yml); libFuzzer
#   is not in that toolchain. Setting B2_BUILD_FUZZERS=ON without a
#   suitable compiler / link flag is a hard configure error, not a
#   silent skip.
#
# HOW TO USE:
#   cmake -S . -B build-fuzz \
#     -DB2_BUILD_FUZZERS=ON \
#     -DCMAKE_CXX_COMPILER=clang++ \
#     -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all"
#   cmake --build build-fuzz
#   ./build-fuzz/fuzz/rbc_text_fuzzer -max_total_len=4096 -max_len=4096
#   ./build-fuzz/fuzz/rbc_verifier_fuzzer -max_total_len=4096 -max_len=4096
#
# TODO (v0 -> v1):
#   - add a b2_corpus seed directory under fuzz/corpus/ with the
#     existing tests/rbc/corpus/*.rbc as seed inputs (maximizes early
#     coverage),
#   - add a T1 instantiation harness (requires plumbing CompiledCode's
#     teardown path to the fuzzer's exit, otherwise the JIT arena
#     leaks across runs),
#   - add a T0 interpreter harness (requires a per-run heap reset;
#     today's Heap.cpp is a process-lifetime bump allocator with no
#     reset API),
#   - wire into OSS-Fuzz once the harness set is broad enough.

option(B2_BUILD_FUZZERS "Build libFuzzer harnesses (opt-in; requires clang + -fsanitize=fuzzer)" OFF)

if(NOT B2_BUILD_FUZZERS)
  return()
endif()

# Sanity: this needs clang's -fsanitize=fuzzer (or a compatible engine).
# We do not hard-require clang because some distros ship a g++ that
# also knows the flag; instead we check the linker can find the symbol.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles("
  extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long);
  int LLVMFuzzerTestOneInput(const unsigned char* d, unsigned long s) { return 0; }
  int main() { return 0; }
" B2_HAS_FUZZER_SUPPORT)
set(CMAKE_REQUIRED_FLAGS "")
if(NOT B2_HAS_FUZZER_SUPPORT)
  message(FATAL_ERROR
    "B2_BUILD_FUZZERS=ON requires a compiler/linker that understands "
    "-fsanitize=fuzzer (clang >= 6). Configure with "
    "-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=\"-fsanitize=fuzzer,...\".")
endif()

add_executable(rbc_text_fuzzer rbc_text_fuzzer.cpp)
target_link_libraries(rbc_text_fuzzer PRIVATE b2::rbc)
target_compile_features(rbc_text_fuzzer PRIVATE cxx_std_26)

add_executable(rbc_verifier_fuzzer rbc_verifier_fuzzer.cpp)
target_link_libraries(rbc_verifier_fuzzer PRIVATE b2::rbc)
target_compile_features(rbc_verifier_fuzzer PRIVATE cxx_std_26)

# Seed corpus: copy the existing RBC corpus into fuzz/corpus/ at build
# time so a fresh fuzzer run starts from real programs, not from empty
# bytes. (This is the single biggest coverage lever for the cost.)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz/corpus/rbc_text)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz/corpus/rbc_verifier)
file(GLOB B2_FUZZ_SEED_RBC ${CMAKE_SOURCE_DIR}/tests/rbc/corpus/*.rbc)
file(GLOB B2_FUZZ_SEED_INTERP ${CMAKE_SOURCE_DIR}/tests/interp/corpus/*.rbc)
foreach(seed IN LISTS B2_FUZZ_SEED_RBC B2_FUZZ_SEED_INTERP)
  configure_file(${seed} ${CMAKE_BINARY_DIR}/fuzz/corpus/rbc_text COPYONLY)
  configure_file(${seed} ${CMAKE_BINARY_DIR}/fuzz/corpus/rbc_verifier COPYONLY)
endforeach()

# Convenience target: run both fuzzers for a short smoke burst.
add_custom_target(fuzz_smoke
  COMMAND
    $<TARGET_FILE:rbc_text_fuzzer>
      -max_total_len=65536 -max_len=4096
      -runs=4096
      ${CMAKE_BINARY_DIR}/fuzz/corpus/rbc_text
  COMMAND
    $<TARGET_FILE:rbc_verifier_fuzzer>
      -max_total_len=65536 -max_len=4096
      -runs=4096
      ${CMAKE_BINARY_DIR}/fuzz/corpus/rbc_verifier
  DEPENDS rbc_text_fuzzer rbc_verifier_fuzzer
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Smoke-run libFuzzer harnesses (4096 runs each)"
  USES_TERMINAL)
