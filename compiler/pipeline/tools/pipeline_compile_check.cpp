// B-2 Pipeline — compile check for the v0 contract surfaces.
//
// WHY THIS FILE EXISTS:
// The v0 contract surfaces under include/b2/pipeline/ are headers-only
// (every function is a no-op; the v0 ships shadow-only). Nothing in
// the tree #includes them yet (the engine's trap handler will be the
// first consumer in the v0 -> v1 transition). This file is the
// mechanical check that the headers parse cleanly under the repo's
// strict flags (-Wall -Wextra -Wpedantic -Wshadow) and that the type
// definitions and APIs are usable from a consumer's perspective.
//
// The file is a TU with a main() that constructs the types and calls
// the no-op APIs; the v0 behavior is "all no-ops return Disabled /
// empty sets". The v0 -> v1 transition will replace this file with
// real unit tests under tests/pipeline/ when the implementations land.
//
// STATUS: v0 — will be replaced by tests/pipeline/ in the v1.

#include <cstdio>

#include "b2/codegen/Instantiate.h"  // CompiledCode (default-constructible)
#include "b2/pipeline/DependencyIndex.h"
#include "b2/pipeline/PartialDeopt.h"
#include "b2/pipeline/Region.h"

int main() {
  // DependencyIndex: the v0 is shadow-only; invalidate returns empty.
  b2::pipeline::DependencyIndex idx;
  const b2::pipeline::DirtySet dirty = idx.invalidate(b2::ir::kInvalidDependency);
  if (!dirty.empty()) {
    std::fprintf(stderr, "DependencyIndex::invalidate should return empty in v0\n");
    return 1;
  }
  idx.record(b2::ir::kInvalidDependency, b2::ir::kInvalidNodeId,
             b2::ir::kInvalidRegion);
  idx.retireMethod(b2::ir::MethodId{0});
  if (idx.size() != 0) {
    std::fprintf(stderr, "DependencyIndex::size should return 0 in v0\n");
    return 1;
  }

  // Region: the v0 struct is defined but no instances are constructed
  // at runtime (the v0 default enable_partial_deopt = false means no
  // regions are built). The struct is constructible here for the
  // compile check.
  b2::pipeline::Region r;
  r.id = b2::ir::kInvalidRegion;
  r.method = b2::ir::MethodId{0};
  r.entryControl = b2::ir::kInvalidNodeId;
  r.exitControls.push_back(b2::ir::kInvalidNodeId);
  r.entryValues.push_back(b2::ir::kInvalidNodeId);
  r.exitValues.push_back(b2::ir::kInvalidNodeId);
  r.entryMemory = b2::ir::kInvalidNodeId;
  r.exitMemory = b2::ir::kInvalidNodeId;
  r.exceptionExit = b2::ir::kInvalidNodeId;
  r.assumptions.push_back(b2::ir::kInvalidDependency);
  r.guards.push_back(b2::ir::kInvalidNodeId);
  r.codeHandle = nullptr;
  r.activeEpoch = 0;
  r.deoptCount = 0;
  r.partialRecompileCount = 0;
  r.failureRate = 0;
  if (r.id != b2::ir::kInvalidRegion) {
    std::fprintf(stderr, "Region.id should be kInvalidRegion\n");
    return 1;
  }
  // RegionSafety: the enum + name function.
  const b2::pipeline::RegionSafety safe = b2::pipeline::RegionSafety::Safe;
  if (b2::pipeline::regionSafetyName(safe)[0] != 's') {
    std::fprintf(stderr, "regionSafetyName(Safe) should start with 's'\n");
    return 1;
  }

  // PartialDeopt: the v0 returns Disabled for every call.
  b2::pipeline::PartialDeoptConfig cfg;
  if (cfg.enable_partial_deopt) {
    std::fprintf(stderr, "PartialDeoptConfig.enable_partial_deopt should default to false\n");
    return 1;
  }
  if (cfg.shadow_mode != true) {
    std::fprintf(stderr, "PartialDeoptConfig.shadow_mode should default to true (shadow-only)\n");
    return 1;
  }
  // The v0 onGuardFailure returns Disabled (feature flag off).
  b2::codegen::CompiledCode cc;  // default-constructed; safe to destroy
  const b2::pipeline::PartialDeoptDecision d =
      b2::pipeline::onGuardFailure(cc, 0, cfg);
  if (d != b2::pipeline::PartialDeoptDecision::Disabled) {
    std::fprintf(stderr, "onGuardFailure should return Disabled in v0 (flag off)\n");
    return 1;
  }
  if (b2::pipeline::partialDeoptDecisionName(d)[0] != 'd') {
    std::fprintf(stderr, "partialDeoptDecisionName(Disabled) should start with 'd'\n");
    return 1;
  }

  // Flip the flag on; the v0 should still return Disabled (shadow-only;
  // the v1 implements the actual flow).
  cfg.enable_partial_deopt = true;
  const b2::pipeline::PartialDeoptDecision d2 =
      b2::pipeline::onGuardFailure(cc, 0, cfg);
  if (d2 != b2::pipeline::PartialDeoptDecision::Disabled) {
    std::fprintf(stderr, "onGuardFailure should return Disabled in v0 (shadow-only even with flag on)\n");
    return 1;
  }

  std::printf("pipeline_compile_check: OK (v0 shadow-only; all no-ops return Disabled/empty)\n");
  return 0;
}
