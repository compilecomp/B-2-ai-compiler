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
// STATUS: v0.3 — IMPLEMENTED. The inverse map is a real
// std::unordered_map<DependencyId, std::vector<Entry>>. `record()`
// stores the association; `invalidate()` returns the dirty set;
// `retireMethod()` removes all associations for a method; `size()`
// returns the count. The implementation is deterministic (Rule 124):
// the dirty set's nodes + regions are sorted by id before return.
//
// The v0.3 is NOT yet wired to the inline pass (the inline pass
// creates ClassHierarchy dependencies via `Graph::addDependency()`
// but does not call `DependencyIndex::record()` — the wiring is the
// v0 -> v1 transition's work). Today the DependencyIndex is exercised
// by unit tests only; the v0 default `enable_partial_deopt = false`
// short-circuits the engine's guard-failure path.

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
//
// Invariants (Rule 124: deterministic):
//   - `nodes` is sorted by NodeId and deduplicated.
//   - `regions` is sorted by RegionId and deduplicated.
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

// One association in the inverse map: a node + a region that depend
// on a DependencyId. The node is the Guard node (or any node carrying
// SpecMeta.dependency); the region is the recompilation region the
// node belongs to (ir::kInvalidRegion if region tracking is not yet
// wired — the v0.3 accepts this and the dirty closure handles it).
struct DependencyEntry {
  ir::NodeId node = ir::kInvalidNodeId;
  ir::RegionId region = ir::kInvalidRegion;
  ir::MethodId method = 0;  // for retireMethod()
};

// The runtime DependencyIndex (Section 2 of the contract).
//
// The inverse map: DependencyId -> list of (node, region, method)
// associations. When an assumption breaks (e.g., a new class is
// loaded that invalidates a ClassHierarchy assumption), the runtime
// calls `invalidate(dep)`; the index returns the dirty set (all
// nodes + regions that depend on `dep`). The caller expands the
// dirty closure (Section 5) and runs the region safety checks
// (Section 6) before attempting partial rebuild.
//
// DETERMINISM (Rule 124): the dirty set's nodes + regions are
// sorted by id before return. The internal storage order is
// insertion order (non-deterministic across runs if the inline
// pass's site order varies), but the RETURNED dirty set is always
// sorted (the caller's dirty closure algorithm depends on sorted
// input for its binary-search membership checks).
//
// THREAD SAFETY: the v0.3 is NOT thread-safe (single-threaded
// today; `b2t2` is synchronous). The v0 -> v1 transition adds
// locking when the multi-threaded compilation base lands
// (`docs/STATUS.md` item 5).
class DependencyIndex {
public:
  DependencyIndex() = default;
  ~DependencyIndex() = default;
  DependencyIndex(const DependencyIndex&) = delete;
  DependencyIndex& operator=(const DependencyIndex&) = delete;

  // Add a (DependencyId, NodeId, RegionId, MethodId) association.
  // Called by the T2 driver (in the v0 -> v1 transition) when it
  // lowers a Guard node whose FrameState's SpecMeta carries a
  // non-kInvalidDependency id. The MethodId is for `retireMethod()`.
  //
  // Duplicate associations (same dep + node + region) are silently
  // deduplicated (the caller may call record() multiple times for
  // the same association; the index keeps one).
  void record(ir::DependencyId dep, ir::NodeId node,
              ir::RegionId region, ir::MethodId method = 0);

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
  // The returned dirty set is sorted by id (Rule 124) and
  // deduplicated.
  [[nodiscard]] DirtySet invalidate(ir::DependencyId dep) const;

  // Per-method teardown: the method's compiled code is being
  // retired; all its dependencies are removed from the index.
  void retireMethod(ir::MethodId m);

  // Telemetry: how many (DependencyId, NodeId, RegionId)
  // associations the index holds. Used by the
  // `deopt-to-reopt latency` and `partial_recompile_success_rate`
  // counters (Section 22).
  [[nodiscard]] std::size_t size() const noexcept;

  // Telemetry: how many distinct DependencyIds the index tracks.
  // (One DependencyId may have multiple associations — e.g., one
  // ClassHierarchy dependency may be referenced by multiple Guard
  // nodes across multiple methods.)
  [[nodiscard]] std::size_t distinctDependencies() const noexcept;

private:
  // The inverse map. Stored as a flat vector of (dep, entry) pairs
  // for deterministic iteration (an unordered_map would be faster
  // for lookup but non-deterministic for iteration; the v0.3
  // prioritizes determinism over performance — the v0 -> v1
  // transition may switch to a sorted vector or a hash map with
  // deterministic iteration).
  //
  // The vector is kept SORTED by (dep, node, region) for:
  //   - deterministic invalidate() output (walk in sorted order),
  //   - O(log n) binary search for duplicate detection in record(),
  //   - O(n) retireMethod() (walk once, remove matching).
  std::vector<std::pair<ir::DependencyId, DependencyEntry>> entries_;
};

} // namespace b2::pipeline
