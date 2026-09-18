#pragma once
// B-2 Pipeline — the partial deopt guard-failure integration.
//
// WHY THIS FILE EXISTS:
// docs/partial_deopt_contract.md Section 9. The guard-failure path
// in compiler/codegen/src/Engine.cpp executeCompiled (lines 900-934
// today) is the integration point for partial deopt. The current
// path does full method deopt to T0 (Rule 96 form); the partial
// deopt layer sits BETWEEN the guard-failure detection and the T0
// fallback, attempting a partial rebuild if safe.
//
// This header is the v0 contract surface: the API the engine's
// trap handler will call. The v0 ships shadow-only: the functions
// are declared but return `EscalateToFullDeopt` for every call.
// The v0 default `enable_partial_deopt = false` short-circuits the
// call entirely; the engine's trap handler goes straight to the
// existing T0 deopt path.
//
// STATUS: v0 design — NO IMPLEMENTATION. The API below is the
// contract surface for the v0 -> v1 transition.

#include <cstdint>

#include "b2/codegen/Tier1.h"        // Tier1RunResult, Tier1Status
#include "b2/interp/Interp.h"        // Frame, ObjRef, Runtime
#include "b2/ir/Graph.h"
#include "b2/ir/Node.h"
#include "b2/pipeline/DependencyIndex.h"
#include "b2/pipeline/DirtyClosure.h"  // IWYU pragma: keep (kDefaultMaxDirtyRegionSize)
#include "b2/pipeline/Region.h"

namespace b2::codegen { struct CompiledCode; }

namespace b2::pipeline {

// Feature flags (Section 21 of the contract). The v0 defaults are
// conservative (everything off). The v0 -> v1 transition flips
// the defaults based on telemetry + the shadow comparison's
// pass rate on the corpus.
struct PartialDeoptConfig {
  bool enable_partial_deopt              = false;
  bool enable_partial_loop_recompile     = false;  // requires OSR (Section 15)
  bool enable_partial_inline_recompile   = false;  // requires ICDG boundary (Section 16)
  bool allow_region_patching             = false;  // requires safepoint handshake (Section 17)

  // Per-method, per-window deopt count budget (Section 19: deopt
  // budget and hysteresis). If a method triggers more than this
  // many deopts in the rolling window, the throttling policy
  // disables partial deopt for the method (the next invocation
  // goes straight to T1 baseline).
  //
  // RATIONALE for the default of 3:
  //   - 1 deopt is normal (the first speculation break for a
  //     method is expected; the recompile adapts).
  //   - 2 deopts is suspicious (the recompile may have picked a
  //     bad speculation; the throttling policy should consider
  //     downgrading).
  //   - 3 deopts is the action threshold (the recompile is
  //     thrashing; stop attempting partial deopt for this method
  //     until the window resets).
  //   - The window size is not configurable at v0; the v0 -> v1
  //     transition adds `deopt_window_ms` based on telemetry.
  std::uint32_t partial_deopt_budget     = 3;

  // Max IR nodes in the dirty closure before the algorithm escalates
  // to full method deopt (Section 19). Defaults to
  // `kDefaultMaxDirtyRegionSize` (single source of truth; the
  // rationale for the value 64 is documented at the constant's
  // declaration in `b2/pipeline/DirtyClosure.h`). Override at
  // runtime via this field; the algorithm takes `max_size` as a
  // parameter at the call site (no global state, Rule 125).
  std::uint32_t max_dirty_region_size    = kDefaultMaxDirtyRegionSize;

  // Verification level: "always" (debug), "sampled" (release).
  // The v0 default is "always" (the IR verifier runs on every
  // partial rebuild; the v0 -> v1 transition may relax to "sampled"
  // in release once golden coverage is trusted).
  enum class VerifyLevel : std::uint8_t { Always, Sampled };
  VerifyLevel verification_level = VerifyLevel::Always;

  // Shadow mode (Section 21): compute partial plan, verify it,
  // do NOT activate, compare with full deopt behavior.
  bool shadow_mode = true;
};

// The guard-failure path's decision (Section 9 of the contract).
enum class PartialDeoptDecision : std::uint8_t {
  // Partial rebuild succeeded; the new region is activated.
  // The T0 fallback is NOT needed (the new region is entered at
  // the next invocation).
  ActivatedNewRegion,

  // Partial rebuild was attempted but failed (unsafe region,
  // verifier failure, budget exceeded, etc.).
  // The caller (the engine's trap handler) escalates to full
  // method deopt (the existing T0 path).
  EscalateToFullDeopt,

  // Partial deopt is disabled (feature flag off, method
  // blacklisted, etc.). The caller escalates to full method deopt.
  Disabled,

  // Shadow mode: the partial plan was computed + verified but
  // not activated. The caller escalates to full method deopt;
  // the shadow comparison is logged (Section 22 telemetry).
  ShadowComputed,
};

[[nodiscard]] constexpr const char* partialDeoptDecisionName(
    PartialDeoptDecision d) noexcept {
  switch (d) {
  case PartialDeoptDecision::ActivatedNewRegion: return "activated";
  case PartialDeoptDecision::EscalateToFullDeopt: return "escalated";
  case PartialDeoptDecision::Disabled: return "disabled";
  case PartialDeoptDecision::ShadowComputed: return "shadow";
  default: return "?";
  }
}

// The guard-failure integration surface (Section 9 of the contract).
//
// The engine's trap handler (Engine.cpp executeCompiled) calls
// this after a guard failure is detected. The function:
//
//   1. captures the deopt state (the FrameState at the guard's pc),
//   2. checks the feature flag + budget,
//   3. marks dependencies dirty via DependencyIndex::invalidate,
//   4. expands the dirty closure (Section 5),
//   5. chooses the minimal safe region (Section 6),
//   6. runs the safety checks (Section 6),
//   7. if unsafe: returns EscalateToFullDeopt (the caller does
//      the T0 fallback),
//   8. if safe: schedules the partial recompile (Section 11),
//      runs the fallback stub (T0 resume) IMMEDIATELY (do NOT
//      wait for recompilation while still executing broken
//      optimized code),
//   9. returns ActivatedNewRegion (or ShadowComputed in shadow mode).
//
// v0: returns Disabled (the v0 default `enable_partial_deopt = false`
// short-circuits the call). The engine's trap handler goes straight
// to the existing T0 deopt path. The v0 -> v1 transition implements
// the full flow.
[[nodiscard]] PartialDeoptDecision onGuardFailure(
    const codegen::CompiledCode& cc,
    std::uint32_t deoptId,
    const PartialDeoptConfig& cfg) {
  (void)cc; (void)deoptId;
  if (!cfg.enable_partial_deopt) {
    return PartialDeoptDecision::Disabled;
  }
  // v0: shadow-only. The v1 implements the full flow.
  return PartialDeoptDecision::Disabled;
}

} // namespace b2::pipeline
