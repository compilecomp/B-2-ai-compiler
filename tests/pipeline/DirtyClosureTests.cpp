// B-2 pipeline tests — the dirty-node closure algorithm.
//
// WHY THIS FILE EXISTS:
// docs/partial_deopt_contract.md Section 5. The v0.1 implementation
// (compiler/pipeline/src/DirtyClosure.cpp) is a monotone fixpoint over
// the IR's def-use chains, with the `semanticsDependOn` predicate
// deciding whether a user's semantics depend on the producer at the
// given input slot. These tests are the mechanical check that:
//
//   - the seed dirty set is deduped + dead-seed-dropped;
//   - the forward closure walks def-use chains through Mem, Data,
//     FrameState, and Parent roles;
//   - the closure does NOT propagate through Ctrl or None roles
//     (control predecessors don't carry data semantics);
//   - side-effecting users (Call*, Store*, Guard) always propagate
//     dirty (the side effect may observe the producer);
//   - the budget cap (max_size) is enforced — the closure returns
//     `budget_exceeded = true` and stops;
//   - the algorithm is deterministic (Rule 124) — the same input
//     produces the same dirty set across runs.
//
// The tests construct minimal IR graphs by hand (the same approach
// the IR team's tests/rbc/ tests use). The v0 -> v1 transition adds
// tests for the backward closure (region expansion through boundary
// nodes) when the region builder lands.

#include "TestHarness.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "b2/ir/Graph.h"
#include "b2/ir/Node.h"
#include "b2/pipeline/DependencyIndex.h"
#include "b2/pipeline/DirtyClosure.h"

namespace {

// Helper: build a minimal "value depends on value" graph IN-PLACE.
// Graph is non-copyable AND non-movable, so we build in-place.
// Returns the node IDs the test cares about (the seed + its forward
// closure).
struct DataChainNodes {
  b2::ir::NodeId start;
  b2::ir::NodeId c1;     // ConstantI 1 (the seed)
  b2::ir::NodeId add1;   // AddI(c1, c1)
  b2::ir::NodeId add2;   // AddI(add1, add1)
};
DataChainNodes makeDataChainGraph(b2::ir::Graph& g) {
  DataChainNodes r;
  r.start = g.make(b2::ir::NodeKind::Start);
  r.c1 = g.constantI(1);
  // AddI's signature is [a, b] (both Data; no Ctrl input per NodeInfo).
  r.add1 = g.make(b2::ir::NodeKind::AddI, {r.c1, r.c1});
  r.add2 = g.make(b2::ir::NodeKind::AddI, {r.add1, r.add1});
  return r;
}

// Helper: build a graph with a Memory-state chain IN-PLACE.
struct MemoryChainNodes {
  b2::ir::NodeId start;
  b2::ir::NodeId ci;     // ClassInit (the mem-state origin)
  b2::ir::NodeId newn;   // New (allocates)
  b2::ir::NodeId store;  // StoreField
  b2::ir::NodeId load;   // LoadField
};
MemoryChainNodes makeMemoryChainGraph(b2::ir::Graph& g) {
  MemoryChainNodes r;
  r.start = g.make(b2::ir::NodeKind::Start);
  // ClassInit: [ctrl, mem]; payload = TypeId
  r.ci = g.make(b2::ir::NodeKind::ClassInit, {r.start, r.start},
                /*payload=*/1);
  // New: [ctrl] -> ref; payload = TypeId
  r.newn = g.make(b2::ir::NodeKind::New, {r.start}, /*payload=*/1);
  // StoreField: [ctrl, mem, obj, value]; payload = FieldId
  r.store = g.make(b2::ir::NodeKind::StoreField,
                   {r.start, r.ci, r.newn, r.newn}, /*payload=*/1);
  // LoadField: [ctrl, mem, obj] -> field value; payload = FieldId
  r.load = g.make(b2::ir::NodeKind::LoadField,
                  {r.start, r.store, r.newn}, /*payload=*/1);
  return r;
}

// Helper: build a graph with a control predecessor (Ctrl role, does NOT
// propagate dirty) IN-PLACE.
struct ControlChainNodes {
  b2::ir::NodeId start;
  b2::ir::NodeId c1;   // ConstantI 1 (the seed for the If's condition)
  b2::ir::NodeId ifn;  // If [start, c1]
  b2::ir::NodeId ift;  // IfTrue [ifn] (Parent role — DOES propagate)
  b2::ir::NodeId reg;  // Region [ift, start] (Ctrl predecessors)
};
ControlChainNodes makeControlChainGraph(b2::ir::Graph& g) {
  ControlChainNodes r;
  r.start = g.make(b2::ir::NodeKind::Start);
  r.c1 = g.constantI(1);
  r.ifn = g.make(b2::ir::NodeKind::If, {r.start, r.c1});
  r.ift = g.make(b2::ir::NodeKind::IfTrue, {r.ifn});
  r.reg = g.make(b2::ir::NodeKind::Region, {r.ift, r.start});
  return r;
}

} // namespace

B2_TEST(pipeline_dirty_closure_empty_seed) {
  // Empty seed → empty closure.
  b2::ir::Graph g; makeDataChainGraph(g);
  b2::pipeline::DirtySet seeds;
  const auto result = b2::pipeline::expandDirtyClosure(g, std::move(seeds));
  CHECK(result.dirty.empty());
  CHECK(!result.budget_exceeded);
}

B2_TEST(pipeline_dirty_closure_seed_dedup) {
  // Duplicate seeds → deduplicated dirty set.
  b2::ir::Graph g;
  const auto ids = makeDataChainGraph(g);
  b2::pipeline::DirtySet seeds;
  seeds.nodes.push_back(ids.c1);
  seeds.nodes.push_back(ids.c1);  // duplicate
  seeds.nodes.push_back(ids.c1);  // triplicate
  const auto result = b2::pipeline::expandDirtyClosure(g, std::move(seeds));
  std::vector<b2::ir::NodeId> nodes = result.dirty.nodes;
  CHECK(std::is_sorted(nodes.begin(), nodes.end()));
  CHECK(std::unique(nodes.begin(), nodes.end()) == nodes.end());
}

B2_TEST(pipeline_dirty_closure_dead_seed_dropped) {
  // A dead seed (a tombstone) is dropped; the closure does not propagate
  // through dead nodes.
  b2::ir::Graph g;
  const auto ids = makeDataChainGraph(g);
  g.killNode(ids.c1);
  b2::pipeline::DirtySet seeds;
  seeds.nodes.push_back(ids.c1);
  const auto result = b2::pipeline::expandDirtyClosure(g, std::move(seeds));
  CHECK(result.dirty.empty());
}

B2_TEST(pipeline_dirty_closure_data_propagation) {
  // Forward closure through Data inputs: dirty ConstantI → AddI → AddI.
  // AddI's signature is [a, b] (both Data); both propagate dirty to the AddI.
  b2::ir::Graph g;
  const auto ids = makeDataChainGraph(g);
  b2::pipeline::DirtySet seeds;
  seeds.nodes.push_back(ids.c1);
  const auto result = b2::pipeline::expandDirtyClosure(g, std::move(seeds));
  const auto& nodes = result.dirty.nodes;
  CHECK(std::is_sorted(nodes.begin(), nodes.end()));
  CHECK(nodes.size() >= 3);
  CHECK(std::binary_search(nodes.begin(), nodes.end(), ids.c1));
  CHECK(std::binary_search(nodes.begin(), nodes.end(), ids.add1));
  CHECK(std::binary_search(nodes.begin(), nodes.end(), ids.add2));
}

B2_TEST(pipeline_dirty_closure_no_data_no_propagation) {
  // A value that has NO users does not propagate (the closure is the seed
  // alone). We construct a graph with an unused ConstantI.
  b2::ir::Graph g;
  const b2::ir::NodeId start = g.make(b2::ir::NodeKind::Start);
  const b2::ir::NodeId unused = g.constantI(42);  // no users
  (void)start;
  b2::pipeline::DirtySet seeds;
  seeds.nodes.push_back(unused);
  const auto result = b2::pipeline::expandDirtyClosure(g, std::move(seeds));
  CHECK(result.dirty.nodes.size() == 1);
  CHECK(result.dirty.nodes[0] == unused);
}

B2_TEST(pipeline_dirty_closure_memory_chain_propagation) {
  // Memory-state chain: dirty ClassInit → StoreField (reads mem) →
  // LoadField (reads mem from StoreField). The Mem role propagates dirty.
  b2::ir::Graph g;
  const auto ids = makeMemoryChainGraph(g);
  b2::pipeline::DirtySet seeds;
  seeds.nodes.push_back(ids.ci);
  const auto result = b2::pipeline::expandDirtyClosure(g, std::move(seeds));
  const auto& nodes = result.dirty.nodes;
  CHECK(nodes.size() >= 3);
  CHECK(std::binary_search(nodes.begin(), nodes.end(), ids.ci));
  CHECK(std::binary_search(nodes.begin(), nodes.end(), ids.store));
  CHECK(std::binary_search(nodes.begin(), nodes.end(), ids.load));
}

B2_TEST(pipeline_dirty_closure_parent_role_propagation) {
  // Parent role: IfTrue/IfFalse/SwitchCase/CallExcept projections propagate
  // dirty from their Parent (the If/Switch/Call* node).
  b2::ir::Graph g;
  const auto ids = makeControlChainGraph(g);
  b2::pipeline::DirtySet seeds;
  seeds.nodes.push_back(ids.ifn);
  const auto result = b2::pipeline::expandDirtyClosure(g, std::move(seeds));
  const auto& nodes = result.dirty.nodes;
  CHECK(std::binary_search(nodes.begin(), nodes.end(), ids.ifn));
  CHECK(std::binary_search(nodes.begin(), nodes.end(), ids.ift));
}

B2_TEST(pipeline_dirty_closure_ctrl_no_propagation) {
  // A Region with Ctrl predecessors does NOT become dirty from its Ctrl
  // inputs. We construct: Start, Region[Start, Start], and seed Start dirty.
  // The Region's inputs are both Ctrl; no propagation.
  b2::ir::Graph g;
  const b2::ir::NodeId start = g.make(b2::ir::NodeKind::Start);
  const b2::ir::NodeId reg = g.make(b2::ir::NodeKind::Region, {start, start});
  (void)reg;
  b2::pipeline::DirtySet seeds;
  seeds.nodes.push_back(start);
  const auto result = b2::pipeline::expandDirtyClosure(g, std::move(seeds));
  const auto& nodes = result.dirty.nodes;
  // The Region should NOT be dirty (Ctrl role doesn't propagate).
  CHECK(!std::binary_search(nodes.begin(), nodes.end(), reg));
  CHECK(std::binary_search(nodes.begin(), nodes.end(), start));
}

B2_TEST(pipeline_dirty_closure_budget_exceeded) {
  // Budget cap: if the closure exceeds max_size, the algorithm returns
  // budget_exceeded = true and stops.
  b2::ir::Graph g;
  const auto ids = makeDataChainGraph(g);
  b2::pipeline::DirtySet seeds;
  seeds.nodes.push_back(ids.c1);
  // max_size = 1: only the seed fits; the closure immediately exceeds.
  const auto result = b2::pipeline::expandDirtyClosure(g, std::move(seeds),
                                                       /*max_size=*/1);
  CHECK(result.budget_exceeded);
  CHECK(result.dirty.nodes.size() >= 1);
}

B2_TEST(pipeline_dirty_closure_determinism) {
  // Determinism (Rule 124): the same input produces the same dirty set
  // across runs.
  b2::ir::Graph g1;
  const auto ids1 = makeDataChainGraph(g1);
  b2::ir::Graph g2;
  const auto ids2 = makeDataChainGraph(g2);
  b2::pipeline::DirtySet seeds1; seeds1.nodes.push_back(ids1.c1);
  b2::pipeline::DirtySet seeds2; seeds2.nodes.push_back(ids2.c1);
  const auto r1 = b2::pipeline::expandDirtyClosure(g1, std::move(seeds1));
  const auto r2 = b2::pipeline::expandDirtyClosure(g2, std::move(seeds2));
  CHECK(r1.dirty.nodes == r2.dirty.nodes);
  CHECK(r1.budget_exceeded == r2.budget_exceeded);
  CHECK(r1.iterations == r2.iterations);
}

B2_TEST(pipeline_dirty_closure_semantics_depend_on_data) {
  // semanticsDependOn: a Data input slot propagates dirty.
  b2::ir::Graph g;
  const auto ids = makeDataChainGraph(g);
  // AddI's signature is [a, b] (both Data; no Ctrl input per NodeInfo).
  // Slot 0 and slot 1 are both Data.
  CHECK(b2::pipeline::semanticsDependOn(g, ids.add1, /*slot=*/0));
  CHECK(b2::pipeline::semanticsDependOn(g, ids.add1, /*slot=*/1));
}

B2_TEST(pipeline_dirty_closure_semantics_depend_on_side_effecting) {
  // semanticsDependOn: a side-effecting user (StoreField) always depends
  // on the producer regardless of the input slot's role.
  b2::ir::Graph g;
  const auto ids = makeMemoryChainGraph(g);
  // StoreField's signature is [ctrl, mem, obj, value] (4 inputs).
  // All slots propagate dirty (Memory-class is side-effecting).
  for (std::uint16_t slot = 0; slot < 4; ++slot) {
    CHECK_MSG(b2::pipeline::semanticsDependOn(g, ids.store, slot),
              ("StoreField should propagate dirty from all input slots; "
               "failed for slot=" + std::to_string(slot)).c_str());
  }
}
