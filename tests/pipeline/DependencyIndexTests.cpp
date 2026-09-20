// B-2 pipeline tests — the runtime DependencyIndex.
//
// WHY THIS FILE EXISTS:
// docs/partial_deopt_contract.md Section 2. The v0.3 implementation
// (compiler/pipeline/src/DependencyIndex.cpp) is the inverse map
// DependencyId -> set<(NodeId, RegionId, MethodId)>. The index is
// the bridge between the IR's per-node SpecMeta.dependency (the
// compile-time scaffolding) and the runtime's dirty-node closure
// (Section 5) + region safety checks (Section 6).
//
// The tests cover:
//   - record() stores associations; invalidate() returns them,
//   - duplicate record() is deduplicated,
//   - invalidate() returns sorted, deduplicated dirty sets,
//   - retireMethod() removes all associations for a method,
//   - distinct dependencies telemetry,
//   - determinism (Rule 124): identical inputs produce identical
//     dirty sets across runs,
//   - the SpecMeta → DependencyId → DependencyIndex wiring path
//     (the speculative-devirtualization scenario: a ClassHierarchy
//     dependency fires when a new subclass is loaded).

#include "TestHarness.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "b2/ir/Graph.h"
#include "b2/ir/Node.h"
#include "b2/pipeline/DependencyIndex.h"
#include "b2/pipeline/Region.h"

B2_TEST(pipeline_dependency_index_empty) {
  // A fresh index has no associations.
  b2::pipeline::DependencyIndex idx;
  CHECK(idx.size() == 0);
  CHECK(idx.distinctDependencies() == 0);
  const auto dirty = idx.invalidate(b2::ir::DependencyId{1});
  CHECK(dirty.empty());
}

B2_TEST(pipeline_dependency_index_record_and_invalidate) {
  // record() stores the association; invalidate() returns it.
  b2::pipeline::DependencyIndex idx;
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::RegionId{20}, b2::ir::MethodId{0});
  CHECK(idx.size() == 1);
  CHECK(idx.distinctDependencies() == 1);
  const auto dirty = idx.invalidate(b2::ir::DependencyId{1});
  CHECK(dirty.nodes.size() == 1);
  CHECK(dirty.nodes[0] == 10);
  CHECK(dirty.regions.size() == 1);
  CHECK(dirty.regions[0] == 20);
}

B2_TEST(pipeline_dependency_index_multiple_associations) {
  // One DependencyId may have multiple associations (multiple Guard
  // nodes across multiple methods depend on the same ClassHierarchy
  // assumption).
  b2::pipeline::DependencyIndex idx;
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::RegionId{20}, b2::ir::MethodId{0});
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{30},
             b2::ir::RegionId{40}, b2::ir::MethodId{0});
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{50},
             b2::ir::RegionId{60}, b2::ir::MethodId{1});
  CHECK(idx.size() == 3);
  CHECK(idx.distinctDependencies() == 1);
  const auto dirty = idx.invalidate(b2::ir::DependencyId{1});
  CHECK(dirty.nodes.size() == 3);
  CHECK(dirty.nodes[0] == 10);
  CHECK(dirty.nodes[1] == 30);
  CHECK(dirty.nodes[2] == 50);
  CHECK(dirty.regions.size() == 3);
}

B2_TEST(pipeline_dependency_index_dedup) {
  // Duplicate record() calls (same dep + node + region) are
  // deduplicated — the caller may call record() multiple times for
  // the same association (e.g., re-lowering the same Guard).
  b2::pipeline::DependencyIndex idx;
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::RegionId{20}, b2::ir::MethodId{0});
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::RegionId{20}, b2::ir::MethodId{0});
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::RegionId{20}, b2::ir::MethodId{0});
  CHECK(idx.size() == 1);  // deduplicated
  const auto dirty = idx.invalidate(b2::ir::DependencyId{1});
  CHECK(dirty.nodes.size() == 1);
}

B2_TEST(pipeline_dependency_index_invalidate_sorted) {
  // invalidate() returns a sorted, deduplicated dirty set (Rule 124).
  // Record associations in NON-sorted order; verify the output is
  // sorted.
  b2::pipeline::DependencyIndex idx;
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{50},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{30},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  const auto dirty = idx.invalidate(b2::ir::DependencyId{1});
  CHECK(std::is_sorted(dirty.nodes.begin(), dirty.nodes.end()));
  CHECK(dirty.nodes.size() == 3);
  CHECK(dirty.nodes[0] == 10);
  CHECK(dirty.nodes[1] == 30);
  CHECK(dirty.nodes[2] == 50);
}

B2_TEST(pipeline_dependency_index_invalidate_unknown_dep) {
  // invalidate() for an unknown DependencyId returns an empty set.
  b2::pipeline::DependencyIndex idx;
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  const auto dirty = idx.invalidate(b2::ir::DependencyId{999});
  CHECK(dirty.empty());
}

B2_TEST(pipeline_dependency_index_retire_method) {
  // retireMethod() removes all associations whose MethodId matches.
  b2::pipeline::DependencyIndex idx;
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{20},
             b2::ir::kInvalidRegion, b2::ir::MethodId{1});
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{30},
             b2::ir::kInvalidRegion, b2::ir::MethodId{1});
  idx.record(b2::ir::DependencyId{2}, b2::ir::NodeId{40},
             b2::ir::kInvalidRegion, b2::ir::MethodId{1});
  CHECK(idx.size() == 4);
  idx.retireMethod(b2::ir::MethodId{1});
  CHECK(idx.size() == 1);  // only method 0's association remains
  const auto dirty = idx.invalidate(b2::ir::DependencyId{1});
  CHECK(dirty.nodes.size() == 1);
  CHECK(dirty.nodes[0] == 10);
}

B2_TEST(pipeline_dependency_index_distinct_deps) {
  // distinctDependencies(): count distinct DependencyIds.
  b2::pipeline::DependencyIndex idx;
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx.record(b2::ir::DependencyId{1}, b2::ir::NodeId{20},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx.record(b2::ir::DependencyId{2}, b2::ir::NodeId{30},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx.record(b2::ir::DependencyId{3}, b2::ir::NodeId{40},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  CHECK(idx.size() == 4);
  CHECK(idx.distinctDependencies() == 3);
}

B2_TEST(pipeline_dependency_index_determinism) {
  // Determinism (Rule 124): the same sequence of record() calls
  // produces the same dirty set across runs (regardless of
  // insertion order — the entries_ vector is kept sorted).
  b2::pipeline::DependencyIndex idx1, idx2;
  // Insert in different orders.
  idx1.record(b2::ir::DependencyId{1}, b2::ir::NodeId{30},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx1.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx1.record(b2::ir::DependencyId{1}, b2::ir::NodeId{20},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx2.record(b2::ir::DependencyId{1}, b2::ir::NodeId{10},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx2.record(b2::ir::DependencyId{1}, b2::ir::NodeId{20},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  idx2.record(b2::ir::DependencyId{1}, b2::ir::NodeId{30},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  const auto d1 = idx1.invalidate(b2::ir::DependencyId{1});
  const auto d2 = idx2.invalidate(b2::ir::DependencyId{1});
  CHECK(d1.nodes == d2.nodes);
  CHECK(d1.regions == d2.regions);
  CHECK(idx1.size() == idx2.size());
}

B2_TEST(pipeline_dependency_index_devirt_scenario) {
  // The speculative-devirtualization scenario: a ClassHierarchy
  // dependency is registered for a TypeProfile Guard (from the
  // inline pass's GuardInline path). When a new subclass is loaded
  // (invalidating the ClassHierarchy assumption), invalidate()
  // returns the Guard node + its region as dirty.
  //
  // This test models the ICDG Phase 2 path (docs/inlining.md
  // section 9): the inline pass creates a Guard node + a
  // ClassHierarchy dependency + a SpecMeta; the DependencyIndex
  // records the association; the runtime fires invalidate() when
  // the assumption breaks.
  b2::pipeline::DependencyIndex idx;
  // The inline pass created:
  //   - Guard node n42 (the TypeProfile guard)
  //   - ClassHierarchy dependency dep=7 (target = the profiled class)
  //   - Region r3 (the guard's recompilation region)
  //   - Method m1 (the method the guard is in)
  const b2::ir::DependencyId classHierDep{7};
  const b2::ir::NodeId guardNode{42};
  const b2::ir::RegionId guardRegion{3};
  const b2::ir::MethodId method{1};
  idx.record(classHierDep, guardNode, guardRegion, method);
  CHECK(idx.size() == 1);
  // The runtime fires invalidate() when a new subclass is loaded.
  const auto dirty = idx.invalidate(classHierDep);
  CHECK(dirty.nodes.size() == 1);
  CHECK(dirty.nodes[0] == guardNode);
  CHECK(dirty.regions.size() == 1);
  CHECK(dirty.regions[0] == guardRegion);
  // The dirty set feeds into expandDirtyClosure() (Section 5) +
  // checkRegionSafety() (Section 6) — the partial deopt path.
}

B2_TEST(pipeline_dependency_index_invalid_dep_ignored) {
  // record() with kInvalidDependency is a no-op (the caller didn't
  // actually register a dependency — the SpecMeta.dependency field
  // is kInvalidDependency for non-speculative nodes).
  b2::pipeline::DependencyIndex idx;
  idx.record(b2::ir::kInvalidDependency, b2::ir::NodeId{10},
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  CHECK(idx.size() == 0);
  const auto dirty = idx.invalidate(b2::ir::kInvalidDependency);
  CHECK(dirty.empty());
}

B2_TEST(pipeline_dependency_index_no_node_no_region_ignored) {
  // record() with both node and region invalid is a no-op (nothing
  // to associate the dependency with).
  b2::pipeline::DependencyIndex idx;
  idx.record(b2::ir::DependencyId{1}, b2::ir::kInvalidNodeId,
             b2::ir::kInvalidRegion, b2::ir::MethodId{0});
  CHECK(idx.size() == 0);
}
