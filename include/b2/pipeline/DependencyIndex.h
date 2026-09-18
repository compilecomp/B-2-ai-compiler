#pragma once
// B-2 Pipeline — the runtime DependencyIndex.
//
// WHY THIS FILE EXISTS:
// docs/partial_deopt_contract.md Section 2. The IR already carries
// `ir::Dependency` (the assumption key, kind + target) and
// `ir::SpecMeta.dependency` (the per-node link to a DependencyId).
// What the IR does NOT have is the global INVERSE map:
// DependencyId -> set<NodeId> / set<RegionId>. The runtime needs
// this to mark nodes dirty when an assumption breaks (Section 5 of
// the contract: dirty-node closure).
//
// This header is the v0 contract surface: the type definitions and
// the API the runtime will call. The implementation is the v0 -> v1
// transition's work; the v0 ships shadow-only (compute the plan,
// verify it, do not activate, see Section 21 of the contract).
//
// The DependencyIndex lives in the pipeline orchestrator
// (compiler/pipeline/) because:
//   - the IR's `Graph` is per-method, per-compile-job; the index is
//     process-global (a class hierarchy change invalidates
//     assumptions across many methods);
//   - the pipeline orchestrator owns the code cache + the assumption
//     tables (it is the natural host);
//   - the v0 stub directory landed in MSG-20260918-004; the
//     DependencyIndex is the first real resident.
//
// STATUS: v0 design — NO IMPLEMENTATION. The API below is the
// contract surface for the v0 -> v1 transition. Calling any of
// these functions today is a no-op (returns empty sets); the v0
// default is `enable_partial_deopt = false` (shadow-only).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "b2/ir/Graph.h"
#include "b2/ir/Node.h"
#include "b2/pipeline/Region.h"  // IWYU pragma: keep (RegionId is defined here)

namespace b2::pipeline {

// The dirty set: nodes + regions whose assumptions have been
// invalidated, awaiting the dirty-node closure expansion
// (Section 5) and the region safety checks (Section 6).
struct DirtySet {
  std::vector<ir::NodeId> nodes;
  std::vector<ir::RegionId> regions;

  [[nodiscard]] bool empty() const noexcept {
    return nodes.empty() && regions.empty();
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return nodes.size() + regions.size();
  }
};

// The runtime DependencyIndex (Section 2 of the contract).
//
// The v0 ships shadow-only: the functions are declared but are
// no-ops. The v0 -> v1 transition implements them; until then,
// callers (the T2 driver's guard-failure path) check the
// `enable_partial_deopt` feature flag and short-circuit to the
// existing T0 deopt path (Section 10 fallback) when the flag is
// false.
class DependencyIndex {
public:
  DependencyIndex() = default;
  ~DependencyIndex() = default;
  DependencyIndex(const DependencyIndex&) = delete;
  DependencyIndex& operator=(const DependencyIndex&) = delete;

  // Add a (DependencyId, NodeId, RegionId) association. Called by
  // the T2 driver when it lowers a Guard node whose FrameState's
  // SpecMeta carries a non-kInvalidDependency id.
  //
  // v0: no-op (shadow-only). The v1 records the association in
  // the inverse map; the v0 default `enable_partial_deopt = false`
  // means the T2 driver doesn't even call this (the Guard is
  // lowered without speculation tracking).
  void record(ir::DependencyId dep, ir::NodeId node,
              ir::RegionId region) {
    (void)dep; (void)node; (void)region;
  }

  // Mark all nodes/regions dependent on `dep` dirty. Called by
  // the runtime when an assumption breaks (e.g., a new class is
  // loaded that invalidates a ClassHierarchy assumption).
  //
  // Returns the dirty set (callers expand to a safe region next,
  // per Section 5). Does NOT immediately recompile anything — it
  // marks the dirty set and lets the guard-failure path (Section
  // 9) decide whether to attempt partial rebuild or escalate to
  // full deopt.
  //
  // v0: returns the empty set (shadow-only). The v1 returns the
  // actual dirty set from the inverse map.
  [[nodiscard]] DirtySet invalidate(ir::DependencyId dep) {
    (void)dep;
    return DirtySet{};
  }

  // Per-method teardown: the method's compiled code is being
  // retired; all its dependencies are removed from the index.
  //
  // v0: no-op. The v1 walks the index and removes all entries
  // whose RegionId belongs to the method.
  void retireMethod(ir::MethodId m) {
    (void)m;
  }

  // Telemetry: how many (DependencyId, NodeId, RegionId)
  // associations the index holds. Used by the
  // `deopt-to-reopt latency` and `partial_recompile_success_rate`
  // counters (Section 22).
  //
  // v0: returns 0. The v1 returns the actual count.
  [[nodiscard]] std::size_t size() const noexcept {
    return 0;
  }
};

} // namespace b2::pipeline
