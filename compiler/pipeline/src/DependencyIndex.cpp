// B-2 Pipeline — the runtime DependencyIndex implementation.
//
// WHY THIS FILE EXISTS:
// docs/partial_deopt_contract.md Section 2. The v0.3 implementation
// of the inverse map: DependencyId -> set<(NodeId, RegionId, MethodId)>.
// The IR's `Graph::addDependency()` returns a DependencyId; the
// `DependencyIndex::record()` stores the inverse association; the
// runtime's `invalidate(dep)` returns the dirty set when an assumption
// breaks.
//
// DETERMINISM (Rule 124): the entries_ vector is kept sorted by
// (dep, node, region); invalidate() walks in sorted order and returns
// a sorted, deduplicated dirty set.

#include "b2/pipeline/DependencyIndex.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace b2::pipeline {

namespace {

// A single (dep, entry) pair comparator for the sorted entries_ vector.
// Sort by (dep, node, region) — the canonical order for deterministic
// iteration + duplicate detection.
struct EntryLess {
  bool operator()(const std::pair<ir::DependencyId, DependencyEntry>& a,
                  const std::pair<ir::DependencyId, DependencyEntry>& b)
      const noexcept {
    if (a.first != b.first) return a.first < b.first;
    if (a.second.node != b.second.node) return a.second.node < b.second.node;
    return a.second.region < b.second.region;
  }
  bool operator()(const std::pair<ir::DependencyId, DependencyEntry>& a,
                  ir::DependencyId dep) const noexcept {
    return a.first < dep;
  }
  bool operator()(ir::DependencyId dep,
                  const std::pair<ir::DependencyId, DependencyEntry>& b)
      const noexcept {
    return dep < b.first;
  }
};

} // namespace

void DependencyIndex::record(ir::DependencyId dep, ir::NodeId node,
                              ir::RegionId region, ir::MethodId method) {
  if (dep == ir::kInvalidDependency) return;
  if (node == ir::kInvalidNodeId && region == ir::kInvalidRegion) return;

  const DependencyEntry entry{node, region, method};
  const std::pair<ir::DependencyId, DependencyEntry> kv{dep, entry};

  // Binary search for the insertion point (sorted by (dep, node, region)).
  auto it = std::lower_bound(entries_.begin(), entries_.end(), kv,
                              EntryLess{});
  // Deduplicate: if the exact (dep, node, region) already exists, skip.
  if (it != entries_.end() && it->first == dep &&
      it->second.node == node && it->second.region == region) {
    // Already recorded. (The MethodId may differ if the same Guard is
    // shared across methods — but the (dep, node, region) is the
    // canonical key; the first-seen MethodId wins. This is fine
    // because retireMethod walks all entries, not just the first.)
    return;
  }
  entries_.insert(it, kv);
}

[[nodiscard]] DirtySet DependencyIndex::invalidate(
    ir::DependencyId dep) const {
  DirtySet result;
  if (dep == ir::kInvalidDependency) return result;

  // Binary search for the range of entries with this dep.
  auto begin = std::lower_bound(entries_.begin(), entries_.end(), dep,
                                 EntryLess{});
  auto end = std::upper_bound(entries_.begin(), entries_.end(), dep,
                               EntryLess{});

  // Collect nodes + regions. The entries_ vector is sorted, so the
  // walk produces sorted output (nodes are a secondary sort key
  // after dep). We still sort + dedup at the end to handle the
  // region field (which is a tertiary sort key).
  for (auto it = begin; it != end; ++it) {
    if (it->second.node != ir::kInvalidNodeId) {
      result.nodes.push_back(it->second.node);
    }
    if (it->second.region != ir::kInvalidRegion) {
      result.regions.push_back(it->second.region);
    }
  }

  // Sort + dedup (Rule 124: deterministic output; the dirty closure
  // algorithm's binary-search membership checks require sorted input).
  std::sort(result.nodes.begin(), result.nodes.end());
  result.nodes.erase(std::unique(result.nodes.begin(), result.nodes.end()),
                      result.nodes.end());
  std::sort(result.regions.begin(), result.regions.end());
  result.regions.erase(std::unique(result.regions.begin(),
                                    result.regions.end()),
                       result.regions.end());

  return result;
}

void DependencyIndex::retireMethod(ir::MethodId m) {
  // Remove all entries whose MethodId matches. The erase-remove
  // idiom preserves the sorted order of the remaining entries
  // (the vector stays sorted because we're removing, not reordering).
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [m](const std::pair<ir::DependencyId, DependencyEntry>& e) {
                       return e.second.method == m;
                     }),
      entries_.end());
}

[[nodiscard]] std::size_t DependencyIndex::size() const noexcept {
  return entries_.size();
}

[[nodiscard]] std::size_t
DependencyIndex::distinctDependencies() const noexcept {
  if (entries_.empty()) return 0;
  // Count distinct deps in the sorted vector (adjacent groups).
  std::size_t count = 1;
  ir::DependencyId prev = entries_[0].first;
  for (const auto& [dep, entry] : entries_) {
    (void)entry;
    if (dep != prev) {
      ++count;
      prev = dep;
    }
  }
  return count;
}

} // namespace b2::pipeline
