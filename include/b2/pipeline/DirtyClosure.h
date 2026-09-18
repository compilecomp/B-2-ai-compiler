#pragma once
// B-2 Pipeline — the dirty-node closure algorithm.
//
// WHY THIS FILE EXISTS:
// docs/partial_deopt_contract.md Section 5. When an assumption breaks
// (a `DependencyId` is invalidated), the runtime `DependencyIndex`
// returns the direct dependents (the seed dirty set). The closure
// algorithm expands these to the transitive set of nodes whose
// semantics depend on the broken assumption:
//
//   DirtySet expandDirtyClosure(const Graph& g, DirtySet seeds,
//                               std::uint32_t max_size);
//
// The closure is the input to:
//   - the region safety checks (Section 6): the minimal safe region
//     contains the closure;
//   - the partial rebuild (Section 11): the new subgraph replaces the
//     closure's nodes;
//   - the deopt budget (Section 19): if the closure exceeds
//     `max_dirty_region_size`, the caller escalates to full method
//     deopt.
//
// STATUS: v0.1 — FORWARD CLOSURE IMPLEMENTED. Backward closure
// (region expansion through boundary nodes) requires region tracking,
// which doesn't exist yet (the v0 contract surfaces landed in
// MSG-20260918-007 but the v1 region builder is open work). The v0.1
// returns the forward closure; the v0 -> v1 transition adds the
// backward closure when the region builder lands.

#include <cstdint>

#include "b2/ir/Graph.h"
#include "b2/ir/Node.h"
#include "b2/pipeline/DependencyIndex.h"
#include "b2/pipeline/Region.h"

namespace b2::pipeline {

// The default max dirty closure size (Section 19 of the contract).
// If the closure exceeds this, the caller escalates to full method
// deopt (Section 10 fallback).
//
// RATIONALE for the default value of 64:
//   - 64 IR nodes is roughly the size of a hot loop body or a small
//     inlined callee — the natural unit of partial recompilation
//     (Section 4 of the contract: "good region candidates"). A
//     closure that stays under 64 covers the common salvage cases
//     (a single failed type profile, a single broken inline, a
//     single invalidated load); a closure that exceeds 64 is
//     likely covering multiple unrelated speculation failures and
//     is no longer "the smallest safe region" — escalate.
//   - 64 is small enough that the partial rebuild's compile cost
//     (lowering + verifying + activating ~64 IR nodes) is bounded
//     by the deopt-to-reopt latency budget (Section 22 telemetry's
//     `deopt_to_reopt_latency_ms`). At ~64 nodes, the rebuild is
//     ~1-3 ms on commodity hardware; above that, the latency grows
//     non-linearly (the verifier's O(n^2) checks dominate).
//   - 64 is large enough that the closure doesn't trivially
//     over-escalate on real Java methods. The interp corpus's
//     largest single-method graph is ~50 nodes today (see
//     `tests/interp/corpus/`); 64 leaves headroom for the larger
//     methods the frontend will lower once the Java stdlib lands
//     (`docs/STATUS.md` item 7).
//
// The default is overridable at runtime via
// `PartialDeoptConfig::max_dirty_region_size` (the runtime config
// passed to `onGuardFailure()`); the algorithm itself takes
// `max_size` as a parameter. The single source of truth for the
// DEFAULT is this constant; `PartialDeoptConfig::max_dirty_region_size`
// defaults to it (no duplicate literal).
inline constexpr std::uint32_t kDefaultMaxDirtyRegionSize = 64;

// The closure result: the expanded dirty set + whether the size budget
// was exceeded. If `budget_exceeded`, the caller MUST escalate to
// full method deopt (Section 10 fallback) — the closure is partial
// and unsafe to rebuild from.
struct DirtyClosureResult {
  DirtySet dirty;
  bool budget_exceeded = false;

  // Telemetry: how many forward-closure iterations the algorithm ran
  // (a monotone fixpoint; the iteration count is bounded by the
  // graph's live node count, but the telemetry surfaces pathological
  // cases for the throttling policy, Section 19).
  std::uint32_t iterations = 0;
};

// Expand the seed dirty set through the graph's def-use chains
// (forward closure). The algorithm:
//
//   dirty = seeds
//   changed = true
//   while changed and not budget_exceeded:
//     changed = false
//     for each node n in dirty.snapshot():
//       for each Use u in graph.usesOf(n):
//         if semanticsDependOn(graph, u.user, u.slot):
//           if u.user not in dirty:
//             dirty.add(u.user)
//             changed = true
//             if dirty.size() > max_size:
//               budget_exceeded = true
//               break
//
// The `semanticsDependOn` predicate is the conservative default: the
// user's semantics depend on the producer if the input slot's role
// is one that propagates dirty (Mem, Data, FrameState, Parent) OR
// the user is a side-effecting node (NodeClass::Call, NodeClass::Memory
// with non-Pure effect, NodeClass::Guard).
//
// The backward closure (Section 5: "if n was a boundary node and input
// contract changed, dirty.add(input)") is NOT implemented at v0.1 —
// it requires region tracking (which doesn't exist yet). The v0.1
// returns the forward closure; the v0 -> v1 transition adds the
// backward closure when the region builder lands.
//
// DETERMINISM (Rule 124): the algorithm is deterministic — the
// iteration order is the graph's node-id order (the IR's nodes_ vector
// is creation-order, stable for the graph's lifetime, Rule 7).
[[nodiscard]] DirtyClosureResult expandDirtyClosure(
    const ir::Graph& g,
    DirtySet seeds,
    std::uint32_t max_size = kDefaultMaxDirtyRegionSize) noexcept;

// The semantics-depends-on predicate (Section 5 of the contract).
//
// Conservative: true unless the input slot's role is Ctrl or None
// (control predecessors and placeholders don't carry data semantics).
// Side-effecting users (NodeClass::Call, NodeClass::Memory with
// non-Pure effect, NodeClass::Guard) always depend on the producer
// regardless of role (the side effect may observe the producer).
//
// Pure value nodes (NodeClass::Value, NodeClass::State) depend on the
// producer only through Data / FrameState / Mem inputs (the standard
// case).
//
// Exposed for unit tests + the v0 -> v1 transition's relaxation
// (the v1 may make the predicate less conservative once the side-effect
// closure is trusted).
[[nodiscard]] bool semanticsDependOn(const ir::Graph& g,
                                      ir::NodeId user,
                                      std::uint16_t slot) noexcept;

} // namespace b2::pipeline
