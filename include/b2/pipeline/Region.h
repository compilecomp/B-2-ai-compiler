#pragma once
// B-2 Pipeline — the recompilation Region.
//
// WHY THIS FILE EXISTS:
// docs/partial_deopt_contract.md Section 4. The IR's `Graph` is the
// whole-method unit of compilation today; the partial deopt layer
// needs a finer-grained unit (the recompilable region) to salvage
// speculation without rebuilding the whole method.
//
// A Region is a safe unit of recompilation: a subgraph with a
// well-defined control entry, control exits, value entries, value
// exits, memory entry, memory exit, and exception exit. The
// boundary is the contract between the region and the rest of the
// optimized graph (or the rest of the method's T1/T0 execution
// path); the partial rebuild (Section 11) builds a NEW region that
// produces the same ExitState (modulo the corrected speculation)
// that the old region would have produced.
//
// STATUS: v0 design — NO IMPLEMENTATION. The types below are the
// contract surface for the v0 -> v1 transition. The v0 ships
// shadow-only; the v0 default `enable_partial_deopt = false` means
// no regions are ever built or activated (the guard-failure path
// escalates to full method deopt, the existing path in
// compiler/codegen/src/Engine.cpp).

#include <cstdint>
#include <vector>

#include "b2/ir/Graph.h"
#include "b2/ir/Node.h"

namespace b2::ir {

// Forward-declared in Node.h? No — RegionId is a pipeline concept,
// not an IR concept. But the contract's `Region` references
// `ir::NodeId` for its control/value/memory fields, so the ir types
// must be visible. RegionId itself is defined HERE (pipeline-owned)
// because adding it to the IR would be a serialization format change
// the v0 contract explicitly defers (Section 1: "the natural home
// for this is a side table in the pipeline orchestrator").
using RegionId = std::uint32_t;
inline constexpr RegionId kInvalidRegion = 0xFFFFFFFFu;

} // namespace b2::ir

namespace b2::pipeline {

// The recompilation region (Section 4 of the contract).
//
// v0: the struct is defined but no instances are constructed (the
// v0 default `enable_partial_deopt = false` means no regions are
// built). The v0 -> v1 transition constructs regions when the T2
// driver installs T2-compiled code (each method's T2 graph is
// partitioned into regions at install time; the partitioning is
// the v1 work).
struct Region {
  ir::RegionId id = ir::kInvalidRegion;

  // The graph this region belongs to (the IR Graph is per-method).
  // Non-owning pointer; the Graph's lifetime is the compile job's
  // (Rule 7: arena allocation with bulk free).
  ir::Graph* graph = nullptr;
  ir::MethodId method = 0;

  // Boundary: the entry control node (Start / IfTrue / IfFalse /
  // SwitchCase / LoopBegin / Region) and the exit control nodes
  // (Return / Unwind / Deopt / Goto-to-outside).
  ir::NodeId entryControl = ir::kInvalidNodeId;
  std::vector<ir::NodeId> exitControls;

  // Boundary: the live values at entry (the data inputs that the
  // region reads from outside) and at exit (the data outputs that
  // the region's successors read).
  std::vector<ir::NodeId> entryValues;
  std::vector<ir::NodeId> exitValues;

  // Boundary: the memory state at entry (the last memory-state
  // node before the region) and at exit (the first memory-state
  // node after the region). Sea-of-nodes memory edges (Rule 7 of
  // docs/ir_spec.md) thread the memory state through the graph.
  ir::NodeId entryMemory = ir::kInvalidNodeId;
  ir::NodeId exitMemory = ir::kInvalidNodeId;

  // Exception exit: the region's exception-continuation node
  // (Unwind / CallExcept / handler Region). The region may throw;
  // the exception exit is part of the boundary contract.
  ir::NodeId exceptionExit = ir::kInvalidNodeId;

  // The interior nodes of the region (the dirty closure's nodes that
  // the region contains). The boundary fields above (entryControl,
  // exitControls, entryValues, exitValues, entryMemory, exitMemory,
  // exceptionExit) describe the region's contract with the rest of the
  // graph; the `nodes` field is the explicit list of interior nodes
  // (for the safety checks in `b2/pipeline/RegionSafety.h` + the
  // partial rebuild in the v0 -> v1 transition).
  //
  // The region builder (v0 -> v1 transition, Section 4 of the contract)
  // constructs this list from the dirty closure's `DirtySet`. The
  // safety checker walks this list to verify the 10 checks from
  // Section 6 (no dangling uses, no unhandled exceptions, etc.).
  //
  // The list is sorted by NodeId (deterministic, Rule 124; matches
  // the DirtySet's invariant from `b2/pipeline/DirtyClosure.h`).
  std::vector<ir::NodeId> nodes;

  // Assumptions + guards: the speculative metadata this region
  // depends on. Mirrors the per-node SpecMeta.dependency but at
  // region granularity (a region's assumptions are the union of
  // its nodes' assumptions; a region's guards are the union of
  // its nodes' Guard nodes).
  std::vector<ir::DependencyId> assumptions;
  std::vector<ir::NodeId> guards;

  // The compiled code handle (the entry point in the JIT arena;
  // null until the region is lowered). The v0 case: the whole
  // method is one region; the code handle is the CompiledCode's
  // exec_base. The v1 case: regions are sub-method units; the
  // code handle is the entry into the region's lowered machine
  // code within the method's CompiledCode.
  void* codeHandle = nullptr;

  // Epoch-based activation (Section 17 of the contract). The
  // region's code is immutable while `activeEpoch` is current; a
  // new region version gets a new epoch, and the old one is
  // retired after no threads are inside it (the safepoint
  // handshake protocol, currently single-threaded).
  //
  // v0: 0 (no epochs; no activation). The v1 uses the IR's
  // `Graph::epoch()` as the source of epoch numbers (the IR
  // already has epoch-tagged replacement, Rule 14).
  std::uint32_t activeEpoch = 0;

  // Telemetry (Section 22): how many times this region has been
  // deopted / partially recompiled. Used by the throttling policy
  // (Section 19) to decide whether to disable partial deopt for
  // the method.
  std::uint32_t deoptCount = 0;
  std::uint32_t partialRecompileCount = 0;
  std::uint32_t failureRate = 0;  // 0..10000 basis points
};

// Region safety check result (Section 6 of the contract).
//
// The 10 checks a region must pass before partial rebuild. The v0
// returns `Unsafe` for every region (the v0 default
// `enable_partial_deopt = false` means no partial rebuild is
// attempted; the guard-failure path escalates to full method deopt).
enum class RegionSafety : std::uint8_t {
  Safe = 0,
  UnsafeBoundary,            // checks 1-2: control entry / exits
  UnsafeDanglingUses,        // check 3: nodes with users outside the region
  UnsafeExceptionEdges,      // check 4: Call* without CallExcept wiring
  UnsafeMemoryState,         // check 5: entry/exit memory
  UnsafeEffectOrdering,      // check 6: memory chain forks
  UnsafePhi,                 // check 7: phi predecessors outside
  UnsafeSideEffectDup,       // check 8: side-effect node in multiple regions
  UnsafeGcRoots,             // check 9: GC-reference liveness mismatch
  UnsafeDeoptState,          // check 10: FrameState pc out of range
  UnsafeTooLarge,            // budget: dirty closure > max_dirty_region_size
  UnsafeBudgetExceeded,      // budget: deopt_count > partial_deopt_budget
};

[[nodiscard]] constexpr const char* regionSafetyName(RegionSafety s) noexcept {
  switch (s) {
  case RegionSafety::Safe: return "safe";
  case RegionSafety::UnsafeBoundary: return "unsafe-boundary";
  case RegionSafety::UnsafeDanglingUses: return "unsafe-dangling-uses";
  case RegionSafety::UnsafeExceptionEdges: return "unsafe-exception-edges";
  case RegionSafety::UnsafeMemoryState: return "unsafe-memory-state";
  case RegionSafety::UnsafeEffectOrdering: return "unsafe-effect-ordering";
  case RegionSafety::UnsafePhi: return "unsafe-phi";
  case RegionSafety::UnsafeSideEffectDup: return "unsafe-side-effect-dup";
  case RegionSafety::UnsafeGcRoots: return "unsafe-gc-roots";
  case RegionSafety::UnsafeDeoptState: return "unsafe-deopt-state";
  case RegionSafety::UnsafeTooLarge: return "unsafe-too-large";
  case RegionSafety::UnsafeBudgetExceeded: return "unsafe-budget-exceeded";
  default: return "?";
  }
}

} // namespace b2::pipeline
