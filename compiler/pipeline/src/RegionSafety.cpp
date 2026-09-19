// B-2 Pipeline — the region safety checks implementation.
//
// WHY THIS FILE EXISTS:
// docs/partial_deopt_contract.md Section 6. The v0.1 implementation
// runs the 10 safety checks from the contract. Checks 1-7 are
// fully implemented (structural checks over the IR + Region);
// checks 8-10 are stubbed (they require machinery that doesn't exist
// yet — see the header for the rationale).
//
// The checks are conservative: if ANY check fails, the verdict is
// Unsafe and the caller escalates to full method deopt (Section 10).
// Never an invalid partial rebuild.

#include "b2/pipeline/RegionSafety.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "b2/ir/Effect.h"
#include "b2/ir/Node.h"

namespace b2::pipeline {

namespace {

// --- helpers ---------------------------------------------------------------

// Is `n` in the sorted `nodes` vector? Binary search (the vector is
// sorted by NodeId per the Region struct's invariant).
[[nodiscard]] bool inRegion(std::vector<ir::NodeId>::const_iterator begin,
                              std::vector<ir::NodeId>::const_iterator end,
                              ir::NodeId n) noexcept {
  return std::binary_search(begin, end, n);
}

// Is `n` a valid control-entry node kind? (Section 6 check 1.)
[[nodiscard]] bool isControlEntryKind(ir::NodeKind k) noexcept {
  switch (k) {
  case ir::NodeKind::Start:
  case ir::NodeKind::IfTrue:
  case ir::NodeKind::IfFalse:
  case ir::NodeKind::SwitchCase:
  case ir::NodeKind::SwitchDefault:
  case ir::NodeKind::LoopBegin:
  case ir::NodeKind::Region:
    return true;
  default:
    return false;
  }
}

// Is `n` a terminal control node? (Section 6 check 2 — exit controls
// must be terminal OR have successors outside the region.)
[[nodiscard]] bool isTerminalControl(ir::NodeKind k) noexcept {
  switch (k) {
  case ir::NodeKind::Return:
  case ir::NodeKind::Unwind:
  case ir::NodeKind::Deopt:
    return true;
  default:
    return false;
  }
}

// Is `n` a Call* node? (Section 6 check 4 — Call* nodes need their
// CallExcept projection wired to exceptionExit.)
[[nodiscard]] bool isCallKind(ir::NodeKind k) noexcept {
  switch (k) {
  case ir::NodeKind::CallStatic:
  case ir::NodeKind::CallVirtual:
  case ir::NodeKind::CallInterface:
  case ir::NodeKind::CallDynamic:
    return true;
  default:
    return false;
  }
}

// Is `n` a memory-state node? (Section 6 check 5 — the memory chain
// entry/exit.) Memory-state nodes have InputRole::Mem in their
// NodeInfo; the check consults the NodeInfo registry via the
// `info()` lookup.
[[nodiscard]] bool isMemoryStateKind(ir::NodeKind k) noexcept {
  const ir::NodeInfo& ni = ir::info(k);
  // Memory-class nodes have a Mem input (the memory-state predecessor).
  // The roles[] array in NodeInfo lists the input roles; check for Mem.
  for (std::uint8_t i = 0; i < ni.numFixed; ++i) {
    if (ni.roles[i] == ir::InputRole::Mem) return true;
  }
  if (ni.variadic && ni.variadicRole == ir::InputRole::Mem) return true;
  return false;
}

// Is `n` a Phi node? (Section 6 check 7 — Phi predecessors must be
// resolvable.)
[[nodiscard]] bool isPhiKind(ir::NodeKind k) noexcept {
  return k == ir::NodeKind::Phi;
}

// --- the 10 checks --------------------------------------------------------
// Each check returns `true` if the region PASSES the check (safe),
// `false` if it FAILS (unsafe). The checker stops at the first
// failing check.

// Check 1: Clearly defined control entry. The region's
// `entryControl` must be a valid control-entry node kind and must
// be in the region's `nodes` list.
[[nodiscard]] bool check1_ControlEntry(const ir::Graph& g,
                                       const Region& r) noexcept {
  if (r.entryControl == ir::kInvalidNodeId) return false;
  if (r.entryControl >= g.nodeCount()) return false;
  const ir::Node& n = g.node(r.entryControl);
  if (n.isDead()) return false;
  if (!isControlEntryKind(n.kind)) return false;
  // entryControl must be in the region's nodes list.
  if (!inRegion(r.nodes.begin(), r.nodes.end(), r.entryControl)) return false;
  return true;
}

// Check 2: Clearly defined control exits. Every exit control must be
// either a terminal (Return/Unwind/Deopt) OR a node whose control
// successor is outside the region. (A Goto to an outside node is the
// latter case; the contract names Return/Unwind/Deopt/Goto-to-outside
// as valid exit controls.)
[[nodiscard]] bool check2_ControlExits(const ir::Graph& g,
                                        const Region& r) noexcept {
  if (r.exitControls.empty()) return false;  // must have at least one exit
  for (ir::NodeId exit : r.exitControls) {
    if (exit == ir::kInvalidNodeId) return false;
    if (exit >= g.nodeCount()) return false;
    const ir::Node& n = g.node(exit);
    if (n.isDead()) return false;
    if (isTerminalControl(n.kind)) continue;  // terminal: OK
    // Non-terminal exit: must be in the region's nodes list (the exit
    // is the last control node IN the region; its successor is outside).
    if (!inRegion(r.nodes.begin(), r.nodes.end(), exit)) return false;
    // The exit's control successors (its users with InputRole::Ctrl)
    // must be outside the region. Walk the use list.
    for (const ir::Use& u : g.usesOf(exit)) {
      if (u.user >= g.nodeCount()) continue;
      const ir::Node& user = g.node(u.user);
      const ir::NodeInfo& ni = ir::info(user.kind);
      // Check if this use is a Ctrl input (slot < numFixed and
      // roles[slot] == Ctrl, OR variadicRole == Ctrl).
      bool isCtrlUse = false;
      if (u.slot < ni.numFixed) {
        isCtrlUse = (ni.roles[u.slot] == ir::InputRole::Ctrl);
      } else if (ni.variadic) {
        isCtrlUse = (ni.variadicRole == ir::InputRole::Ctrl);
      }
      if (isCtrlUse && inRegion(r.nodes.begin(), r.nodes.end(), u.user)) {
        // A control successor INSIDE the region — the exit is not a
        // real boundary exit.
        return false;
      }
    }
  }
  return true;
}

// Check 3: No dangling uses leaving the region. Every node in the
// region that has a user OUTSIDE the region must be in the
// `exitValues` list (the region's exit boundary contract).
[[nodiscard]] bool check3_DanglingUses(const ir::Graph& g,
                                        const Region& r) noexcept {
  for (ir::NodeId n : r.nodes) {
    if (n >= g.nodeCount()) continue;
    const ir::Node& node = g.node(n);
    if (node.isDead()) continue;
    for (const ir::Use& u : g.usesOf(n)) {
      if (u.user >= g.nodeCount()) continue;
      if (inRegion(r.nodes.begin(), r.nodes.end(), u.user)) continue;
      // The user is OUTSIDE the region. Is `n` in exitValues?
      if (!inRegion(r.exitValues.begin(), r.exitValues.end(), n)) {
        return false;  // dangling use
      }
    }
  }
  return true;
}

// Check 4: No unhandled exceptions. Every Call* node in the region
// must have its CallExcept projection wired to `exceptionExit` OR
// to a handler inside the region. (The contract says "every Call*
// has its CallExcept projection wired to the region's exceptionExit,
// OR every FrameState in the region has a handler-compatible pc".)
// The v0.1 implements the structural version: Call* nodes in the
// region must have their CallExcept user either inside the region
// or equal to exceptionExit.
[[nodiscard]] bool check4_ExceptionEdges(const ir::Graph& g,
                                          const Region& r) noexcept {
  for (ir::NodeId n : r.nodes) {
    if (n >= g.nodeCount()) continue;
    const ir::Node& node = g.node(n);
    if (node.isDead()) continue;
    if (!isCallKind(node.kind)) continue;
    // Find the CallExcept projection of this Call*.
    bool callExceptWired = false;
    for (const ir::Use& u : g.usesOf(n)) {
      if (u.user >= g.nodeCount()) continue;
      const ir::Node& user = g.node(u.user);
      if (user.kind != ir::NodeKind::CallExcept) continue;
      // The CallExcept projection is wired to exceptionExit OR is
      // inside the region.
      if (u.user == r.exceptionExit) {
        callExceptWired = true;
        break;
      }
      if (inRegion(r.nodes.begin(), r.nodes.end(), u.user)) {
        callExceptWired = true;
        break;
      }
    }
    if (!callExceptWired) return false;
  }
  return true;
}

// Check 5: Memory state entry/exit. `entryMemory` must be the unique
// memory-state predecessor OUTSIDE the region; `exitMemory` must be
// the unique memory-state successor OUTSIDE the region. (The
// contract says "the entry memory's effect chain reaches the exit
// memory" — the v0.1 checks the boundary existence + uniqueness.)
//
// If the region has NO memory-state nodes (no Load*/Store*/Monitor*/
// New*/ClassInit/Call* nodes in `r.nodes`), the memory state flows
// through unchanged — entryMemory and exitMemory should both be
// invalid OR both point to the same external node. The check skips
// the boundary verification in this case (no memory chain to preserve).
[[nodiscard]] bool check5_MemoryState(const ir::Graph& g,
                                       const Region& r) noexcept {
  // First: does the region contain any memory-state nodes?
  bool hasMemoryNodes = false;
  for (ir::NodeId n : r.nodes) {
    if (n >= g.nodeCount()) continue;
    const ir::Node& node = g.node(n);
    if (node.isDead()) continue;
    if (isMemoryStateKind(node.kind)) {
      hasMemoryNodes = true;
      break;
    }
  }
  if (!hasMemoryNodes) {
    // No memory-state nodes in the region. entryMemory and exitMemory
    // should be equal (the memory state flows through unchanged) OR
    // both invalid (the region doesn't touch memory). Either is safe.
    if (r.entryMemory == r.exitMemory) return true;
    // Both invalid OR both equal — safe.
    if (r.entryMemory == ir::kInvalidNodeId &&
        r.exitMemory == ir::kInvalidNodeId) return true;
    // Mismatched: one valid, one invalid. Unsafe.
    return false;
  }

  if (r.entryMemory == ir::kInvalidNodeId) return false;
  if (r.entryMemory >= g.nodeCount()) return false;
  // entryMemory must be OUTSIDE the region (it's the boundary).
  if (inRegion(r.nodes.begin(), r.nodes.end(), r.entryMemory)) return false;

  if (r.exitMemory == ir::kInvalidNodeId) return false;
  if (r.exitMemory >= g.nodeCount()) return false;
  // exitMemory must be OUTSIDE the region (it's the boundary).
  if (inRegion(r.nodes.begin(), r.nodes.end(), r.exitMemory)) return false;

  // Count memory-state nodes in the region that have their Mem input
  // from outside the region. There must be at most ONE such external
  // memory predecessor (the entryMemory).
  ir::NodeId externalMemPred = ir::kInvalidNodeId;
  for (ir::NodeId n : r.nodes) {
    if (n >= g.nodeCount()) continue;
    const ir::Node& node = g.node(n);
    if (node.isDead()) continue;
    if (!isMemoryStateKind(node.kind)) continue;
    // Walk this node's inputs; find the Mem input.
    const ir::NodeInfo& ni = ir::info(node.kind);
    for (std::uint8_t s = 0; s < node.numInputs; ++s) {
      const ir::InputRole role = (s < ni.numFixed)
          ? ni.roles[s]
          : (ni.variadic ? ni.variadicRole : ir::InputRole::None);
      if (role != ir::InputRole::Mem) continue;
      const ir::NodeId memPred = g.input(n, s);
      if (memPred >= g.nodeCount()) continue;
      if (inRegion(r.nodes.begin(), r.nodes.end(), memPred)) continue;
      // External memory predecessor found.
      if (memPred != r.entryMemory) return false;  // not unique
      if (externalMemPred != ir::kInvalidNodeId &&
          externalMemPred != memPred) {
        return false;  // multiple external memory predecessors
      }
      externalMemPred = memPred;
    }
  }
  // At least one external memory predecessor must exist (the entry).
  if (externalMemPred == ir::kInvalidNodeId) return false;
  return true;
}

// Check 6: Effect ordering preserved. The memory chain from
// entryMemory to exitMemory is linear (no forks). The v0.1 checks
// that every memory-state node in the region has exactly ONE Mem
// input and that the chain doesn't fork (a memory-state node with
// multiple Mem successors inside the region would be a fork).
//
// v0.1 note: this is a conservative structural check. The full
// effect-ordering check (per `b2/ir/Effect.h`'s reorder table) is
// the v0 -> v1 transition's work; the v0.1 only checks the chain's
// linearity.
[[nodiscard]] bool check6_EffectOrdering(const ir::Graph& g,
                                          const Region& r) noexcept {
  // For each memory-state node in the region, count how many Mem
  // inputs it has. Must be exactly one (a linear chain).
  for (ir::NodeId n : r.nodes) {
    if (n >= g.nodeCount()) continue;
    const ir::Node& node = g.node(n);
    if (node.isDead()) continue;
    if (!isMemoryStateKind(node.kind)) continue;
    const ir::NodeInfo& ni = ir::info(node.kind);
    std::uint8_t memInputs = 0;
    for (std::uint8_t s = 0; s < node.numInputs; ++s) {
      const ir::InputRole role = (s < ni.numFixed)
          ? ni.roles[s]
          : (ni.variadic ? ni.variadicRole : ir::InputRole::None);
      if (role == ir::InputRole::Mem) ++memInputs;
    }
    if (memInputs > 1) return false;  // forked memory chain
  }
  return true;
}

// Check 7: Phi nodes resolvable. Every Phi in the region has its
// predecessors' values known (either as entry values or as
// region-internal nodes). The v0.1 checks that every Phi input is
// either in the region's `nodes` or in `entryValues`.
[[nodiscard]] bool check7_PhiResolvable(const ir::Graph& g,
                                          const Region& r) noexcept {
  for (ir::NodeId n : r.nodes) {
    if (n >= g.nodeCount()) continue;
    const ir::Node& node = g.node(n);
    if (node.isDead()) continue;
    if (!isPhiKind(node.kind)) continue;
    // Phi's inputs are its predecessors' values (Data role).
    // All must be either in the region or in entryValues.
    for (std::uint16_t s = 0; s < node.numInputs; ++s) {
      const ir::NodeId in = g.input(n, s);
      if (in >= g.nodeCount()) continue;
      if (inRegion(r.nodes.begin(), r.nodes.end(), in)) continue;
      if (inRegion(r.entryValues.begin(), r.entryValues.end(), in)) continue;
      return false;  // unresolvable phi input
    }
  }
  return true;
}

// Check 8: No side-effect duplication. Every side-effecting node in
// the region is NOT in any other region.
//
// v0.1 STUB: requires a region registry (to check that no
// side-effecting node is in multiple regions). The region registry
// doesn't exist yet (the v0 contract surfaces landed in
// MSG-20260918-007; the registry is the v0 -> v1 transition's
// work). The stub returns `true` (safe) — the v0.1 is shadow-only;
// the v0 -> v1 transition must implement this check before flipping
// `enable_partial_deopt = true`.
[[nodiscard]] bool check8_SideEffectDup(const ir::Graph& /*g*/,
                                         const Region& /*r*/) noexcept {
  // TODO(MSG-20260918-007 v0 -> v1): walk the region registry; for
  // each side-effecting node in `r.nodes`, verify it's not in any
  // other region's `nodes` list.
  return true;  // stub: safe (v0.1 is shadow-only)
}

// Check 9: No ambiguous GC roots. The region's GC-reference liveness
// at entry matches the caller's expectation (the T1 baseline's stack
// map format from `docs/baseline_contract.md` SS4).
//
// v0.1 STUB: requires GC map machinery (the stack map format). The
// GC maps don't exist yet (the JIT hardening doc `docs/jit_hardening.md`
// documents the v0/v1 plan). The stub returns `true` (safe) — the
// v0.1 is shadow-only; the v0 -> v1 transition must implement this
// check before flipping `enable_partial_deopt = true`.
[[nodiscard]] bool check9_GcRoots(const ir::Graph& /*g*/,
                                   const Region& /*r*/) noexcept {
  // TODO(MSG-20260918-007 v0 -> v1): consult the T1 baseline's
  // stack map for the entry control's RBC pc; verify the GC-
  // reference liveness matches the region's entryValues.
  return true;  // stub: safe (v0.1 is shadow-only)
}

// Check 10: Reconstructable deopt state. Every Guard in the region
// has a FrameState whose pc is in the method's RBC code range.
//
// v0.1 STUB: requires the method's RBC code range (to verify the
// FrameState's pc is in range). The Region struct today carries
// `method` (the MethodId) but not the method's code range; the v0.1
// can't verify the pc without looking up the method's code. The
// stub returns `true` (safe) — the v0.1 is shadow-only; the v0 -> v1
// transition must implement this check (probably by passing the
// method's code range to the checker, or by carrying it in the
// Region struct) before flipping `enable_partial_deopt = true`.
[[nodiscard]] bool check10_DeoptState(const ir::Graph& /*g*/,
                                       const Region& /*r*/) noexcept {
  // TODO(MSG-20260918-007 v0 -> v1): for each Guard in r.nodes,
  // find its FrameState input (the trailing FrameState per NodeInfo);
  // verify the FrameState's pc is in the method's code range.
  return true;  // stub: safe (v0.1 is shadow-only)
}

} // namespace

[[nodiscard]] RegionSafetyResult checkRegionSafety(
    const ir::Graph& g, const Region& r) noexcept {
  RegionSafetyResult result;
  result.checkedNodeCount = static_cast<std::uint32_t>(r.nodes.size());

  // Run the 10 checks in order; stop at the first failure.
  if (!check1_ControlEntry(g, r)) {
    result.verdict = RegionSafety::UnsafeBoundary;
    result.failedCheck = 1;
    return result;
  }
  if (!check2_ControlExits(g, r)) {
    result.verdict = RegionSafety::UnsafeBoundary;
    result.failedCheck = 2;
    return result;
  }
  if (!check3_DanglingUses(g, r)) {
    result.verdict = RegionSafety::UnsafeDanglingUses;
    result.failedCheck = 3;
    return result;
  }
  if (!check4_ExceptionEdges(g, r)) {
    result.verdict = RegionSafety::UnsafeExceptionEdges;
    result.failedCheck = 4;
    return result;
  }
  if (!check5_MemoryState(g, r)) {
    result.verdict = RegionSafety::UnsafeMemoryState;
    result.failedCheck = 5;
    return result;
  }
  if (!check6_EffectOrdering(g, r)) {
    result.verdict = RegionSafety::UnsafeEffectOrdering;
    result.failedCheck = 6;
    return result;
  }
  if (!check7_PhiResolvable(g, r)) {
    result.verdict = RegionSafety::UnsafePhi;
    result.failedCheck = 7;
    return result;
  }
  if (!check8_SideEffectDup(g, r)) {
    result.verdict = RegionSafety::UnsafeSideEffectDup;
    result.failedCheck = 8;
    return result;
  }
  if (!check9_GcRoots(g, r)) {
    result.verdict = RegionSafety::UnsafeGcRoots;
    result.failedCheck = 9;
    return result;
  }
  if (!check10_DeoptState(g, r)) {
    result.verdict = RegionSafety::UnsafeDeoptState;
    result.failedCheck = 10;
    return result;
  }

  // All 10 checks passed (or stubbed safe).
  result.verdict = RegionSafety::Safe;
  result.failedCheck = 0;
  return result;
}

} // namespace b2::pipeline
