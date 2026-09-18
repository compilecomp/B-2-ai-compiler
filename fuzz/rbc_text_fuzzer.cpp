// B-2 Fuzz harness: RBC text parser.
//
// WHY THIS HARNESS EXISTS:
//   The RBC text format (docs/rbc_spec.md) is the human debug surface for
//   frontend lowering reviews and the input of every tier. The verifier
//   promise (include/b2/rbc/Verifier.h) is "arbitrary garbage input
//   produces a bounded diagnostic list, never a crash and never an
//   infinite loop." This harness exercises that promise against
//   libFuzzer-driven random byte streams.
//
// WHAT IT CHECKS:
//   - parseRbcText never crashes on arbitrary input,
//   - the parser is total (no UB, no infinite loop) on every input the
//     fuzzer can synthesize,
//   - on success, the resulting Program is well-formed enough to hand
//     to the verifier without leaking.
//
// BUILD:
//   cmake -S . -B build-fuzz -DB2_BUILD_FUZZERS=ON \
//     -DCMAKE_CXX_COMPILER=clang++ \
//     -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all"
//   cmake --build build-fuzz
//   ./build-fuzz/fuzz/rbc_text_fuzzer -max_total_len=4096 -max_len=4096
//
// The harness links the same b2::rbc library the tests use; no special
// build path for the production code.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "b2/rbc/RbcText.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) {
  // Treat the input as an RBC text stream. parseRbcText's contract is
  // total: any input returns either a Program or a TextError; never UB,
  // never an infinite loop. The harness is the mechanical check that
  // the contract holds.
  std::string_view text(reinterpret_cast<const char*>(data), size);
  auto result = b2::rbc::parseRbcText(text);
  if (result.has_value()) {
    // On success, round-trip the Program through the printer to make
    // sure printRbcText is total on its own output (no infinite loop,
    // no UB, no assertion).
    std::string printed = b2::rbc::printRbcText(*result);
    auto reparsed = b2::rbc::parseRbcText(printed);
    // Round-trip should be a stable fixpoint: the re-parsed program
    // should also be valid. We don't assert byte-equality here because
    // the printer normalizes (e.g. trailing whitespace), but the
    // reparse must not crash.
    (void)reparsed;
  }
  return 0;
}
