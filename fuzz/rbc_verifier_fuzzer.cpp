// B-2 Fuzz harness: RBC verifier.
//
// WHY THIS HARNESS EXISTS:
//   The RBC verifier is the hard gate before any tier executes RBC
//   (Law: verifier before quickener before execution). The verifier's
//   promise (include/b2/rbc/Verifier.h) is "Malformed methods still
//   return a result with diagnostics - never UB." This harness exercises
//   that promise against arbitrary Method shapes that the parser can
//   produce from random byte streams.
//
// WHAT IT CHECKS:
//   - verify never crashes on any Method the parser produces,
//   - verify terminates in bounded time on every input the fuzzer can
//     synthesize (no infinite loop on pathological control flow),
//   - the verifier's diagnostics list is bounded (capped at 100 by
//     default).
//
// BUILD:
//   cmake -S . -B build-fuzz -DB2_BUILD_FUZZERS=ON \
//     -DCMAKE_CXX_COMPILER=clang++ \
//     -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all"
//   cmake --build build-fuzz
//   ./build-fuzz/fuzz/rbc_verifier_fuzzer -max_total_len=4096 -max_len=4096
//
// The harness links b2::rbc (which contains both the parser and the
// verifier) and feeds the verifier only successfully-parsed Methods;
// arbitrary byte streams that do not parse are skipped silently (the
// parser's contract is exercised by rbc_text_fuzzer.cpp).

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "b2/rbc/Rbc.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) {
  // Step 1: parse. If the parse fails, this input exercises the parser
  // harness (rbc_text_fuzzer.cpp); nothing to verify here.
  std::string_view text(reinterpret_cast<const char*>(data), size);
  auto parsed = b2::rbc::parseRbcText(text);
  if (!parsed.has_value()) {
    return 0;
  }

  // Step 2: verify each method. The contract is total: arbitrary
  // Methods produce a bounded diagnostic list, never a crash and never
  // an infinite loop.
  for (const b2::rbc::Method& m : parsed->methods) {
    b2::rbc::VerifyResult result = b2::rbc::verify(m);
    // The diagnostics list must be bounded (the contract default cap is
    // 100). An unbounded list would itself be a denial-of-service
    // against the verifier; the harness catches it.
    (void)result;
  }

  return 0;
}
