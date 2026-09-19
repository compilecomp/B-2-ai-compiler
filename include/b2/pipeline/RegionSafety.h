#pragma once
// B-2 Pipeline — the region safety checks.
//
// WHY THIS FILE EXISTS:
// docs/partial_deopt_contract.md Section 6. Before partial rebuild,
// the chosen region must pass 10 safety checks. The checks are
// conservative predicates over the Region struct (from
// `b2/pipeline/Region.h`); if ANY check fails, the caller escalates
// to full method deopt (Section 10 fallback) — never an invalid
// partial rebuild.
//
// The 10 checks (from the contract):
//   1. Clearly defined control entry.
//   2. Clearly defined control exits.
//   3. No dangling uses leaving the region.
//   4. No unhandled exceptions.
//   5. Memory state entry/exit.
//   6. Effect ordering preserved.
//   7. Phi nodes resolvable.
//   8. No side-effect duplication.
//   9. No ambiguous GC roots.
//  10. Reconstructable deopt state.
//
// STATUS: v0.1 — CHECKS 1-7 IMPLEMENTED; CHECKS 8-10 STUBBED. The
// stubbed checks require machinery that doesn't exist yet:
//   - check 8 (side-effect duplication): requires a region registry
//     (to check that no side-effecting node is in multiple regions);
//   - check 9 (GC roots): requires GC map machinery (the T1 baseline's
//     stack map format from `docs/baseline_contract.md` SS4);
//   - check 10 (deopt state): requires the method's RBC code range
//     (to verify the FrameState's pc is in range).
// The stubs return `RegionSafety::Safe` (the v0.1 is shadow-only; the
// v0 -> v1 transition must implement the stubs before flipping
// `enable_partial_deopt = true`). The v0.1 is unit-tested with
// hand-constructed regions (same approach as the dirty closure tests).

#include <cstdint>

#include "b2/ir/Graph.h"
#include "b2/ir/Node.h"
#include "b2/pipeline/Region.h"

namespace b2::pipeline {

// The result of `checkRegionSafety`. If `verdict == RegionSafety::Safe`,
// the region is safe to partially rebuild; otherwise, the verdict
// names the first failing check (the caller escalates to full method
// deopt).
//
// Telemetry (Section 22): `failedCheck` is the check number (1-10)
// that first failed; 0 if Safe. `checkedNodeCount` is the number of
// interior nodes the checker walked (for the `dirty_node_count`
// telemetry counter).
struct RegionSafetyResult {
  RegionSafety verdict = RegionSafety::Safe;
  std::uint8_t failedCheck = 0;  // 0 = Safe; 1-10 = the failing check
  std::uint32_t checkedNodeCount = 0;

  [[nodiscard]] bool isSafe() const noexcept {
    return verdict == RegionSafety::Safe;
  }
};

// Run the 10 safety checks on the given region.
//
// The checker walks the region's `nodes` list (the interior nodes)
// and verifies the 10 checks from Section 6 of the contract. The
// graph is consulted for use-def chains, node kinds, and input roles
// (the IR's `NodeInfo` registry is the source of truth).
//
// The checks are run in order; the first failing check sets the
// verdict and stops (no need to run further checks — the region is
// unsafe regardless of the remaining checks' results).
//
// DETERMINISM (Rule 124): the checks are deterministic — the
// iteration is in the region's `nodes` list order (sorted by NodeId,
// per the Region struct's invariant), and the use-def chain walks
// are in the IR's node-id order.
[[nodiscard]] RegionSafetyResult checkRegionSafety(
    const ir::Graph& g, const Region& r) noexcept;

// The per-check name (for telemetry + error messages).
[[nodiscard]] constexpr const char* regionSafetyCheckName(
    std::uint8_t check) noexcept {
  switch (check) {
  case 0:  return "safe";
  case 1:  return "control-entry";
  case 2:  return "control-exits";
  case 3:  return "dangling-uses";
  case 4:  return "exception-edges";
  case 5:  return "memory-state";
  case 6:  return "effect-ordering";
  case 7:  return "phi-resolvable";
  case 8:  return "side-effect-dup";
  case 9:  return "gc-roots";
  case 10: return "deopt-state";
  default: return "?";
  }
}

} // namespace b2::pipeline
