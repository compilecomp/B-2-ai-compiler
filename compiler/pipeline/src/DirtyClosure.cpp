// B-2 Pipeline — the dirty-node closure algorithm implementation.
//
// WHY THIS FILE EXISTS:
// docs/partial_deopt_contract.md Section 5. The v0 contract surfaces
// landed in MSG-20260918-007 (include/b2/pipeline/*.h); this is the
// v0.1 implementation of the FORWARD closure (the backward closure
// requires region tracking, which doesn't exist yet).
//
// The algorithm is a monotone fixpoint over the graph's def-use chains.
// The IR's `Graph::usesOf(n)` returns `SmallVector<Use, 3>` (Rule 19);
// `NodeInfo` (from `b2/ir/Node.h`) carries the input-role registry that
// `semanticsDependOn` consults. The IR's nodes_ vector is creation-
// order, stable for the graph's lifetime (Rule 7), so the iteration is
// deterministic (Rule 124).

#include "b2/pipeline/DirtyClosure.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "b2/ir/Effect.h"
#include "b2/ir/Node.h"

namespace b2::pipeline {

namespace {

// Returns the InputRole of slot `slot` for node `user` in graph `g`.
// Mirrors the NodeInfo registry's slot-to-role mapping:
//   - slots [0, numFixed)               -> roles[slot]
//   - slots [numFixed, numInputs - FS?) -> variadicRole
//   - slot numInputs - 1 (if hasFrameState) -> FrameState
[[nodiscard]] ir::InputRole roleOfSlot(const ir::Graph& g,
                                        ir::NodeId user,
                                        std::uint16_t slot) noexcept {
  const ir::Node& n = g.node(user);
  if (n.numInputs == 0) {
    return ir::InputRole::None;
  }
  const ir::NodeInfo& info = ir::info(n.kind);
  // FrameState is the mandatory trailing input.
  if (info.hasFrameState && slot == static_cast<std::uint16_t>(n.numInputs - 1)) {
    return ir::InputRole::FrameState;
  }
  if (slot < info.numFixed) {
    return info.roles[slot];
  }
  // Variadic region (or beyond; the verifier rejects beyond-numInputs slots,
  // but the closure is defensive: returns the variadic role for any
  // non-FS slot above numFixed).
  if (info.variadic) {
    return info.variadicRole;
  }
  return ir::InputRole::None;
}

// A side-effecting node's semantics always depend on the producer
// (the side effect may observe the producer's value or speculation).
// Section 13 of the contract: "If a dirty region contains calls/stores/
// allocations, be conservative."
[[nodiscard]] bool isSideEffectingUser(const ir::Node& n) noexcept {
  const ir::NodeInfo& info = ir::info(n.kind);
  switch (info.cls) {
    case ir::NodeClass::Call:
      // All Call* nodes: the call may observe its arguments' values and
      // speculation (e.g., a CallVirtual's receiver's type profile is
      // speculation the call depends on).
      return true;
    case ir::NodeClass::Memory:
      // All Memory-class nodes (Load*, Store*, Monitor*, New*, ClassInit,
      // MemBar): the memory effect may observe the producer's value or
      // speculation. Pure-value nodes (arithmetic on loaded values) are
      // NodeClass::Value, not Memory.
      // Exception: a memory node with EffectKind::Pure is impossible by
      // construction (the registry marks memory nodes as non-Pure), but
      // the check is defensive.
      return info.effect != ir::EffectKind::Pure;
    case ir::NodeClass::Guard:
      // Guard nodes: the guard's behavior (deopt or not) depends on the
      // condition's value. The condition is a Data input (slot 1 per
      // NodeKind::Guard's signature [ctrl, cond(int), framestate]).
      return true;
    case ir::NodeClass::Control:
    case ir::NodeClass::TypeOp:
    case ir::NodeClass::Value:
    case ir::NodeClass::State:
      return false;
  }
  return false;
}

} // namespace

[[nodiscard]] bool semanticsDependOn(const ir::Graph& g,
                                      ir::NodeId user,
                                      std::uint16_t slot) noexcept {
  const ir::Node& n = g.node(user);
  // Dead users can't propagate dirty (they're tombstones; the verifier
  // rejects dangling inputs to dead nodes anyway).
  if (n.isDead()) {
    return false;
  }
  // Side-effecting users always depend on the producer.
  if (isSideEffectingUser(n)) {
    return true;
  }
  // Non-side-effecting users: depend on the producer only through
  // the input slot's role. Ctrl and None don't propagate.
  const ir::InputRole role = roleOfSlot(g, user, slot);
  switch (role) {
    case ir::InputRole::Mem:
      // Memory state predecessor: the memory chain. If the producer's
      // speculation is invalidated, the memory state is invalidated.
      return true;
    case ir::InputRole::Data:
      // Data value: the user's value depends on the producer's value.
      return true;
    case ir::InputRole::FrameState:
      // Deopt state: the FrameState's locals depend on the producer's
      // value (the deopt reconstruction needs the producer's value or
      // its speculation's corrected value).
      return true;
    case ir::InputRole::Parent:
      // Projection source (IfTrue/IfFalse of an If, SwitchCase of a
      // Switch, CallExcept of a Call*): the projection's behavior is
      // determined by the parent. If the parent's behavior changes
      // (e.g., the If's condition is invalidated), the projection's
      // behavior changes too.
      return true;
    case ir::InputRole::Ctrl:
      // Control predecessor: the control token already flowed; the
      // predecessor's identity doesn't change the projection's
      // semantics (the projection is determined by its Parent, not by
      // which Region's control token arrived).
      return false;
    case ir::InputRole::None:
      // Placeholder / unused slot.
      return false;
  }
  return false;
}

[[nodiscard]] DirtyClosureResult expandDirtyClosure(
    const ir::Graph& g,
    DirtySet seeds,
    std::uint32_t max_size) noexcept {
  DirtyClosureResult result;
  result.dirty = std::move(seeds);

  // The dirty membership lookup: a sorted vector of NodeIds for
  // deterministic O(log n) membership + O(1) amortized append.
  // Determinism (Rule 124): the iteration is in node-id order (the
  // IR's nodes_ vector is creation-order), and the membership set
  // is kept sorted for stable iteration.
  std::vector<ir::NodeId> dirtyNodes = std::move(result.dirty.nodes);
  std::sort(dirtyNodes.begin(), dirtyNodes.end());
  dirtyNodes.erase(std::unique(dirtyNodes.begin(), dirtyNodes.end()),
                    dirtyNodes.end());

  // Drop dead seeds (a tombstone in the seed set is a no-op; the
  // closure doesn't propagate through dead nodes).
  dirtyNodes.erase(
      std::remove_if(dirtyNodes.begin(), dirtyNodes.end(),
                     [&](ir::NodeId n) {
                       return n >= g.nodeCount() || g.node(n).isDead();
                     }),
      dirtyNodes.end());

  // Monotone fixpoint: keep iterating until no new dirty nodes are
  // added OR the budget is exceeded.
  bool changed = true;
  while (changed && !result.budget_exceeded) {
    changed = false;
    ++result.iterations;

    // Snapshot the dirty set's size at the start of the iteration;
    // we iterate over the [0, snapshot_size) range of dirtyNodes
    // (newly-added nodes during this iteration are appended beyond
    // snapshot_size and processed in the next iteration).
    const std::size_t snapshot_size = dirtyNodes.size();

    for (std::size_t i = 0; i < snapshot_size; ++i) {
      const ir::NodeId n = dirtyNodes[i];
      if (n >= g.nodeCount() || g.node(n).isDead()) {
        continue;
      }
      // Forward closure: walk the def-use chain.
      const auto& uses = g.usesOf(n);
      for (const ir::Use& u : uses) {
        if (u.user >= g.nodeCount() || g.node(u.user).isDead()) {
          continue;
        }
        if (!semanticsDependOn(g, u.user, u.slot)) {
          continue;
        }
        // Binary search for deterministic membership.
        auto it = std::lower_bound(dirtyNodes.begin(), dirtyNodes.end(),
                                    u.user);
        if (it != dirtyNodes.end() && *it == u.user) {
          continue;  // already dirty
        }
        // Not present: insert (keeps the vector sorted).
        dirtyNodes.insert(it, u.user);
        changed = true;
        if (dirtyNodes.size() > max_size) {
          result.budget_exceeded = true;
          break;
        }
      }
      if (result.budget_exceeded) {
        break;
      }
    }
  }

  result.dirty.nodes = std::move(dirtyNodes);
  return result;
}

} // namespace b2::pipeline
