# B-2 Partial Deopt Contract (v0 design)

Owner: cross-team contract — Passes Team (the pipeline orchestrator hosting
the DependencyIndex + Region structures), Codegen Team (the guard-failure
integration point in `executeCompiled`), IR Team (the `Dependency` /
`SpecMeta` / `FrameStateDesc` scaffolding the contract builds on),
Interpreter Team (the T0 deopt fallback the contract escalates to),
Integrator (this doc); see `docs/teams/README.md`

```text
Normative reference: docs/laws.md
If this document conflicts with docs/laws.md, docs/laws.md wins.
```

**Status: v0 design — not implemented.** This document is the contract
for the partial deopt layer that sits on top of the v0 method-granularity
invalidation already described in `docs/deopt_backend.md` Section 17
("Invalidations and dependency manager"). The IR already carries the
scaffolding (`ir::Dependency`, `ir::SpecMeta.dependency`,
`ir::FrameStateDesc`, `ir::Replacement`); the runtime does not yet
have the global `DependencyIndex`, the `Region` structure, the
dirty-node closure algorithm, the partial rebuild, or the atomic
swap machinery. The T2 execution driver (landed in
`MSG-20260918-005`) is the consumer that motivates this work —
without partial deopt, every guard failure in T2 code is a full
method deopt to T0, which makes speculation too expensive to be
worth it.

The contract is **production-safe by construction**: partial deopt
is never required for correctness. The interpreter/baseline path is
always available; partial deopt is a performance salvage mechanism
that runs only when its safety checks pass. When any check fails,
the system escalates to full method deopt to T0 (Rule 96 form,
unchanged from v0).

---

## 0. The core rule

> **Never mutate the live optimized sea-of-nodes graph in place.**

Partial deopt = dependency-driven invalidation + minimal safe region
recompilation + atomic swap. The live optimized graph, the live
machine code, the live stack frames, and the live GC maps are
immutable while any thread is inside them. When an assumption
breaks, the system:

1. Records assumptions per optimized node/region (compile-time
   metadata; the IR already carries `SpecMeta.dependency`).
2. When an assumption breaks, marks affected nodes dirty (the
   runtime `DependencyIndex` — the inverse map from
   `DependencyId` → `set<NodeId>` / `set<RegionId>`, not yet
   implemented).
3. Expands dirty nodes to the smallest **safe recompilation
   region** (the `Region` structure + closure algorithm, not yet
   implemented).
4. Rebuilds only that region (the partial rebuild, not yet
   implemented).
5. Keeps unaffected optimized code alive (copy-on-write; the
   epoch-based activation machinery, not yet implemented).
6. If anything is uncertain, falls back to full deopt to T0 (the
   existing path in `compiler/codegen/src/Engine.cpp`
   `executeCompiled` lines 900-934, unchanged).

The contract below is the design for steps 2-5. Steps 1 and 6
already exist.

---

## 1. What the IR already carries (the compile-time scaffolding)

The B-2 IR (`include/b2/ir/Graph.h`, `include/b2/ir/Node.h`) already
has the per-node metadata the partial deopt layer needs at
compile time. The v0 implementation work is the runtime structures
that consume this metadata, not the metadata itself.

| User's design field | B-2 IR field | Status |
|---|---|---|
| `assumptions[]` per node | `SpecMeta.dependency` (`DependencyId`); `Node.specMeta` (`SpecMetaId` + 1) | **exists** |
| `guards[]` per node | `NodeKind::Guard` with `payload = GuardKind`, `payload2 = DeoptId`; the `Guard` node is the runtime check | **exists** |
| `effect_class` per node | `EffectKind` (from `b2/ir/Effect.h`); `NodeInfo.effect` is the registry row | **exists** |
| `region_id` per node | — | **MISSING** (the v0 work: a side table `NodeId → RegionId`, or a `Node.region_id` field) |
| `debug_info` for deopt reconstruction | `FrameStateDesc` (`method`, `pc`, `caller`, `vobjOffset`, `vobjCount`) | **exists** |
| `source_origin` | `FrameStateDesc.pc` (RBC pc; the RBC text is the source) | **exists** |
| Replacement log (for replay/debugging) | `ir::Replacement` (`oldNode`, `newNode`, `epoch`); `Graph::replacements()` | **exists** |
| Epoch tagging (Rule 14) | `Node.epoch`, `Graph::epoch()`, `Graph::nextEpoch()` | **exists** |
| Use-def chains (for dirty-node closure) | `Graph::usesOf(n)` returns `SmallVector<Use, 3>` | **exists** |
| Speculation metadata (Rule 122) | `SpecMeta` (`Kind`, `Source`, `confidence`, `guard`, `deoptTarget`, `cost`, `dependency`, `rollback`) | **exists** |
| Assumption key (`Dependency`) | `ir::Dependency` (`Kind ∈ {ClassHierarchy, MethodBody, FieldFinality, ProfileCounter, StaticProof}`, `target`) | **exists** |
| Invalidation API | `Graph::addDependency(dep)` returns `DependencyId` | **exists** |

What the IR does NOT have (the v0 work):

- A global `DependencyIndex` mapping `DependencyId → set<NodeId>`
  (the inverse map). Today `addDependency` returns an id but
  nothing in the runtime tracks which nodes depend on which
  assumption. The runtime needs the inverse map to mark nodes dirty
  when an assumption breaks.
- A `Region` structure (entry/exit control, entry/exit values, entry/
  exit memory state, exception exit, assumptions, guards, code
  handle). Today the IR has no notion of a recompilable region; the
  whole graph is the unit of compilation.
- A `region_id` per node (so the dirty-node closure can identify
  which region a dirty node belongs to). The natural home for this
  is a side table in the pipeline orchestrator (`compiler/pipeline/`),
  not a new field on `Node` (which is fixed 40 bytes; adding a field
  is a serialization format change).

---

## 2. The DependencyIndex (runtime structure — v0.3 implemented)

The runtime `DependencyIndex` is the inverse map from
`DependencyId → set<NodeId>` (and, transitively,
`→ set<RegionId>`). It lives in the pipeline orchestrator
(`compiler/pipeline/`) because:

- the pipeline orchestrator is the natural host for compile-time
  metadata that survives across runs (the orchestrator owns the
  code cache + the assumption tables);
- the IR's `Graph` is per-method, per-compile-job; the
  `DependencyIndex` is process-global (a class hierarchy change
  invalidates assumptions across many methods);
- the pipeline orchestrator is currently a v0 stub (landed in
  `MSG-20260918-004`); the `DependencyIndex` is the first real
  resident of `compiler/pipeline/`.

### v0.3 implementation status

**Status: v0.3 — implemented.** The inverse map is a real
`std::vector<std::pair<DependencyId, DependencyEntry>>` kept sorted
by `(dep, node, region)` for:

- deterministic `invalidate()` output (walk in sorted order;
  Rule 124),
- O(log n) binary search for duplicate detection in `record()`,
- O(n) `retireMethod()` (walk once, remove matching).

The API:

- `record(dep, node, region, method)` — stores the association.
  Duplicate `(dep, node, region)` calls are deduplicated (the
  caller may call `record()` multiple times for the same Guard
  re-lowering; the index keeps one). The `method` field is for
  `retireMethod()`.
- `invalidate(dep)` — returns the `DirtySet` (sorted, deduplicated
  nodes + regions that depend on `dep`). The dirty set feeds into
  `expandDirtyClosure()` (Section 5) + `checkRegionSafety()`
  (Section 6).
- `retireMethod(method)` — removes all associations whose MethodId
  matches (the method's compiled code is being retired).
- `size()` — the total association count (telemetry).
- `distinctDependencies()` — the distinct DependencyId count
  (telemetry; one dep may have multiple associations).

THREAD SAFETY: the v0.3 is NOT thread-safe (single-threaded today;
`b2t2` is synchronous). The v0 → v1 transition adds locking when
the multi-threaded compilation base lands (`docs/STATUS.md` item 5).

The v0.3 is NOT yet wired to the inline pass. The inline pass
creates `ClassHierarchy` dependencies via `Graph::addDependency()`
but does NOT call `DependencyIndex::record()` — the wiring is the
v0 → v1 transition's work (the inline pass needs to also pass the
DependencyIndex to `record()` when it creates a GuardInline guard).
Today the DependencyIndex is exercised by unit tests only.

12 unit tests in `tests/pipeline/DependencyIndexTests.cpp` cover:
empty index, record + invalidate, multiple associations per dep,
dedup, sorted output, unknown dep, retireMethod, distinct deps
telemetry, determinism (Rule 124), the speculative-devirtualization
scenario (a ClassHierarchy dependency fires when a new subclass is
loaded → invalidate() returns the Guard node + its region), invalid
dep ignored, no-node-no-region ignored.

The speculative-devirtualization scenario test
(`pipeline_dependency_index_devirt_scenario`) models the ICDG Phase 2
path: the inline pass creates a Guard node + a ClassHierarchy
dependency + a SpecMeta; the DependencyIndex records the association;
the runtime fires `invalidate()` when the assumption breaks; the
dirty set feeds into the partial deopt path.

The `DirtySet` is the input to the dirty-node closure algorithm
(Section 5). The `invalidate(dep)` call is the runtime event
hook (Section 6 of the user's design); it does NOT immediately
recompile anything — it marks the dirty set and lets the
guard-failure path (Section 9) decide whether to attempt partial
rebuild or escalate to full deopt.

---

## 3. The assumption keys (already in the IR; the contract is the runtime hook)

The `ir::Dependency::Kind` enum is the assumption key taxonomy:

| `ir::Dependency::Kind` | User's design key | When it breaks |
|---|---|---|
| `ClassHierarchy` (`target = TypeId`) | `TypeAssumption(class_id, shape_version)` | a new subclass is loaded that may override a method or implement an interface |
| `MethodBody` (`target = MethodId`) | `MethodVersion(method_id, version)` | method redefinition (JVMTI / hot-swapping; not yet implemented) |
| `FieldFinality` (`target = FieldId`) | `FieldFinal(class_id, field_id)` | a final field is written (reflection; not yet implemented) |
| `ProfileCounter` (`target = profile site id`) | (the profile-driven assumptions: `InlineTarget`, `ArrayBoundsHoisting`, `NoException`, `PrototypeChain`) | the profile counter drifts beyond the speculation's confidence band |
| `StaticProof` (`target = proof artifact id`) | (the T3-style proof; not yet implemented) | the proof's premises are invalidated |

The user's design lists more granular keys (`TypeAssumption`,
`FieldFinal`, `MethodVersion`, `InlineTarget`,
`ArrayBoundsHoisting`, `NoException`, `AliasAssumption`,
`PrototypeChain`). The B-2 IR collapses these into 5 kinds; the
granularity difference is intentional (Rule 16: the IR carries no
strings; the key's `target` is an opaque id resolved by the
frontend's tables). If a future revision needs finer granularity
(e.g., distinguishing `InlineTarget` from `PrototypeChain`), the
`Dependency::Kind` enum grows; the `DependencyIndex` is agnostic
to the kind (it indexes by `DependencyId`, not by kind).

---

## 4. The Region structure (v0 work)

A `Region` is a safe unit of recompilation. The user's design
lists the region boundary contract; the B-2 version maps to
existing structures:

```text
// The contract surface (to land in compiler/pipeline/Region.h):

struct Region {
  ir::RegionId id;

  // The graph this region belongs to (the IR Graph is per-method).
  ir::Graph* graph;
  ir::MethodId method;

  // Boundary: the entry control node (Start / IfTrue / IfFalse /
  // SwitchCase / LoopBegin / Region) and the exit control nodes
  // (Return / Unwind / Deopt / Goto-to-outside).
  ir::NodeId entryControl;
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
  ir::NodeId entryMemory;
  ir::NodeId exitMemory;

  // Exception exit: the region's exception-continuation node
  // (Unwind / CallExcept / handler Region). The region may throw;
  // the exception exit is part of the boundary contract.
  ir::NodeId exceptionExit;

  // Assumptions + guards: the speculative metadata this region
  // depends on. Mirrors the per-node SpecMeta.dependency but at
  // region granularity (a region's assumptions are the union of
  // its nodes' assumptions; a region's guards are the union of
  // its nodes' Guard nodes).
  std::vector<ir::DependencyId> assumptions;
  std::vector<ir::NodeId> guards;

  // The compiled code handle (the entry point in the JIT arena;
  // null until the region is lowered).
  void* codeHandle;

  // Epoch-based activation (Section 17 of the user's design).
  // The region's code is immutable while `activeEpoch` is current;
  // a new region version gets a new epoch, and the old one is
  // retired after no threads are inside it (the safepoint
  // handshake protocol, currently single-threaded).
  std::uint32_t activeEpoch;
};
```

A region can be recompiled independently if (Section 6 of the
user's design):

- all incoming values are known (the entry values' IRTypes are
  pinned by the FrameState at the entry control),
- all outgoing values are producible (the exit values' IRTypes
  are pinned by the region's successor FrameStates),
- memory state is correctly threaded (the entry memory's effect
  chain is preserved through to the exit memory),
- exceptions are handled (the exception exit is wired),
- control transfer is well-defined (the entry control's
  successors match the region's exit controls).

Good region candidates (Section 4 of the user's design):

- function body (the whole method; trivially safe; the v0 case),
- hot loop (the LoopBegin + its body, terminated by LoopEnd; OSR
  entry/exit required — Section 15),
- inlined callee (the inlined subgraph bounded by the call site
  and the call's continuation — Section 16),
- basic block cluster (a Region + its dominated sub-Regions),
- exception handler scope (a handler Region + its body),
- OSR loop body (the loop body at the OSR entry point; a special
  case of "hot loop" with a non-standard entry).

---

## 5. Dirty-node closure (v0.1 — forward closure implemented)

When an assumption breaks, the `DependencyIndex::invalidate(dep)`
returns the direct dependents. The closure algorithm expands
these to the transitive set of nodes whose semantics depend on
the broken assumption (Section 5 of the user's design):

```text
DirtySet expandDirtyClosure(DependencyIndex& idx, DirtySet seeds) {
  DirtySet dirty = seeds;
  bool changed = true;
  while (changed) {
    changed = false;
    for (NodeId n : dirty.snapshot()) {
      // Forward closure: users whose semantics depend on n.
      for (Use u : graph.usesOf(n)) {
        if (semanticsDependOn(u.user, n, u.slot) &&
            !dirty.contains(u.user)) {
          dirty.add(u.user);
          changed = true;
        }
      }
      // Backward closure: if n was a boundary node (entry value,
      // entry memory), its input contract may have changed.
      if (isBoundaryNode(n)) {
        for (NodeId in : graph.inputsOf(n)) {
          if (!dirty.contains(in)) {
            dirty.add(in);
            changed = true;
          }
        }
      }
    }
  }
  return dirty;
}
```

The `semanticsDependOn(user, n, slot)` predicate is the
conservative default: true unless the user is a pure value node
(ConstantI, Parameter, Phi) whose semantics are not affected by
the producer's value. The exact predicate is per-NodeKind; the
registry row in `NodeInfo` carries the role (Ctrl / Mem / Data /
FrameState / Parent), which is most of what the predicate needs.

The closure is bounded by the dirty-region size limit
(`max_dirty_region_size`, Section 19); if the closure exceeds the
limit, the system escalates to full method deopt (Section 10).

### v0.1 implementation status

**Status: v0.1 — forward closure implemented; backward closure
open work.** The v0.1 implementation landed in
`MSG-20260918-008` (`compiler/pipeline/src/DirtyClosure.cpp` +
`include/b2/pipeline/DirtyClosure.h`). The forward closure
walks def-use chains through:

- `InputRole::Data` — pure value nodes' operands propagate dirty
  (a value node's value depends on its operand's value).
- `InputRole::Mem` — memory-state predecessors propagate dirty
  (the memory chain must be rebuilt if any producer's speculation
  breaks).
- `InputRole::FrameState` — deopt state propagates dirty (the
  FrameState's locals are the deopt reconstruction; if the
  producer's speculation breaks, the deopt state is invalidated).
- `InputRole::Parent` — projection sources propagate dirty
  (IfTrue/IfFalse of an If; SwitchCase of a Switch; CallExcept of
  a Call*. The projection's behavior is determined by its Parent).

The closure does NOT propagate through `InputRole::Ctrl` (control
predecessors don't carry data semantics; the control token already
flowed) or `InputRole::None` (placeholder / unused slots).

Side-effecting users (`NodeClass::Call`, `NodeClass::Memory` with
non-`Pure` effect, `NodeClass::Guard`) ALWAYS propagate dirty
regardless of the input slot's role — the side effect may observe
the producer's value or speculation (Section 13: "If a dirty region
contains calls/stores/allocations, be conservative").

The `semanticsDependOn` predicate is exposed in the header
(`include/b2/pipeline/DirtyClosure.h`) for unit tests + the v0 → v1
transition's relaxation (the v1 may make the predicate less
conservative once the side-effect closure is trusted).

The budget cap (`max_size`) is enforced: if the closure exceeds the
limit, the algorithm sets `DirtyClosureResult::budget_exceeded = true`
and stops. The caller checks the flag and escalates to full method
deopt (Section 10).

The backward closure (region expansion through boundary nodes)
requires region tracking, which doesn't exist yet (the v0 contract
surfaces for `Region` landed in `MSG-20260918-007`; the region
BUILDER is the v0 → v1 transition's work). The v0.1 returns the
forward closure; the v0 → v1 transition adds the backward closure
when the region builder lands.

The v0.1 is unit-tested in `tests/pipeline/DirtyClosureTests.cpp`
(12 tests: empty seed, seed dedup, dead-seed drop, Data/Mem/Parent
propagation, Ctrl no-propagation, side-effecting-user rule, budget
cap, determinism, the `semanticsDependOn` predicate). The tests
construct minimal IR graphs by hand (the same approach as
`tests/ir/`).

DETERMINISM (Rule 124): the algorithm is deterministic — the
iteration is in the graph's node-id order (the IR's `nodes_` vector
is creation-order, stable for the graph's lifetime, Rule 7), and
the membership set is a sorted vector for stable O(log n)
membership + O(1) amortized append.

---

## 6. Region safety checks (v0 algorithm)

Before partial rebuild, the chosen region must pass the safety
checks (Section 6 of the user's design). The B-2 versions:

1. **Clearly defined control entry** — the region's
   `entryControl` is a single node (Start / IfTrue / IfFalse /
   SwitchCase / LoopBegin / Region) with a single predecessor
   outside the region.
2. **Clearly defined control exits** — every exit control is
   either a Return / Unwind / Deopt (terminal) or a Goto to a
   node outside the region.
3. **No dangling uses leaving the region** — every node in the
   region that has a user outside the region is in the
   `exitValues` list.
4. **No unhandled exceptions** — every Call* node in the region
   has its CallExcept projection wired to the region's
   `exceptionExit`, OR every FrameState in the region has a
   handler-compatible `pc` (the RBC verifier's exception-table
   check, already enforced).
5. **Memory state entry/exit** — the region's `entryMemory` is
   the unique memory-state predecessor outside the region; the
   `exitMemory` is the unique memory-state successor outside the
   region.
6. **Effect ordering preserved** — the region's effect chain
   (Section 13) is a single linear chain from `entryMemory` to
   `exitMemory` (no aliases, no forks). Forks require rebuilding
   the memory Phi; if the memory Phi is outside the region, the
   region is unsafe.
7. **Phi nodes resolvable** — every Phi in the region has its
   predecessors' values known (either as entry values or as
   region-internal nodes).
8. **No side-effect duplication** — every side-effecting node
   (Call*, Store*, MonitorEnter/Exit, New, ClassInit) in the
   region is NOT in any other region (no double-execution).
9. **No ambiguous GC roots** — the region's GC-reference liveness
   at the entry (from the FrameState) matches the caller's
   expectation. The T1 baseline's stack map format (one bit per
   T0-frame slot; see `docs/baseline_contract.md` SS4) is the
   contract surface.
10. **Reconstructable deopt state** — every Guard in the region
    has a FrameState whose `pc` is in the method's RBC code range
    (already enforced by the IR verifier, Rule 40).

If any check fails, the system escalates to full method deopt
(Section 10). The checks are conservative; the v0 → v1 transition
may relax them as the test corpus grows.

### v0.1 implementation status

**Status: v0.1 — checks 1-7 implemented; checks 8-10 stubbed.**
The v0.1 implementation landed in `MSG-20260918-010`
(`compiler/pipeline/src/RegionSafety.cpp` +
`include/b2/pipeline/RegionSafety.h`). The checker runs the 10
checks in order; the first failing check sets the verdict and
stops (no need to run further checks — the region is unsafe
regardless of the remaining checks' results).

The implemented checks (1-7):

1. **Control entry** — `entryControl` is a valid control-entry
   node kind (Start / IfTrue / IfFalse / SwitchCase / SwitchDefault /
   LoopBegin / Region) AND is in the region's `nodes` list.
2. **Control exits** — every exit control is either terminal
   (Return / Unwind / Deopt) OR a node whose control successors
   (users with `InputRole::Ctrl`) are OUTSIDE the region.
3. **Dangling uses** — for every node in `nodes`, every use
   outside the region is in `exitValues`.
4. **Exception edges** — for every Call* node in `nodes`, its
   CallExcept projection is either inside the region OR equal
   to `exceptionExit`.
5. **Memory state** — if the region has memory-state nodes,
   `entryMemory` is the unique external Mem predecessor and
   `exitMemory` is the unique external Mem successor. If the
   region has NO memory-state nodes, `entryMemory` and
   `exitMemory` must be equal OR both invalid (the memory state
   flows through unchanged).
6. **Effect ordering** — every memory-state node in `nodes` has
   at most ONE Mem input (no forked memory chain). The full
   effect-ordering check (per `b2/ir/Effect.h`'s reorder table)
   is the v0 → v1 transition's work.
7. **Phi resolvable** — every Phi node in `nodes` has all its
   inputs either in `nodes` OR in `entryValues`.

The stubbed checks (8-10) return `RegionSafety::Safe` (the v0.1
is shadow-only; the stubs don't escalate). The v0 → v1 transition
must implement them before flipping `enable_partial_deopt = true`:

8. **Side-effect duplication** — requires a region registry (to
   check that no side-effecting node is in multiple regions).
9. **GC roots** — requires GC map machinery (the T1 baseline's
   stack map format from `docs/baseline_contract.md` SS4).
10. **Deopt state** — requires the method's RBC code range (to
    verify the FrameState's pc is in range).

The checker is unit-tested in `tests/pipeline/RegionSafetyTests.cpp`
(12 tests: simple safe region, check 1 invalid entry kind, check 1
entry not in region, check 2 empty exits, check 3 dangling use,
check 4 Call* without CallExcept, check 5 memory boundary smoke,
check 6 effect ordering smoke, check 7 phi resolvable smoke, stubs
8-10 don't escalate, determinism, the `regionSafetyCheckName`
helper). The tests construct minimal IR graphs + Region structs by
hand (same approach as `tests/pipeline/DirtyClosureTests.cpp`).

DETERMINISM (Rule 124): the checks are deterministic — the
iteration is in the region's `nodes` list order (sorted by NodeId,
per the Region struct's invariant), and the use-def chain walks
are in the IR's node-id order.

---

## 7. Copy-on-write; never patch live nodes (v0 invariant)

The live optimized graph, the live machine code, the live stack
frames, and the live GC maps are immutable while any thread is
inside them (Section 7 of the user's design). The B-2 v0 enforces
this by:

- Building a NEW region (new IR subgraph + new machine code) in
  the compiler thread (currently the calling thread; the v0 is
  synchronous — `b2t2` is single-threaded today, see
  `docs/STATUS.md` item 5).
- Atomically swapping the region's `codeHandle` at a safepoint
  (in the v0 single-threaded case, at the next method invocation;
  the v1 multi-threaded case needs the safepoint handshake protocol
  described in `docs/deopt_backend.md` Section 17).
- Retiring the old region after no threads are inside it (the
  `activeEpoch` machinery; the v0 case is trivial — only the
  calling thread can be inside).

Do not mutate:

- live nodes (the IR's `replaceNode` rewrites use-def chains but
  the old node stays as a tombstone with `Dead|Replaced` flags;
  partial deopt builds a NEW region, it does not call `replaceNode`
  on live nodes),
- live machine code (the JIT arena's W^X discipline — see
  `docs/jit_hardening.md` for the v0 W^X state and the v1
  execute-only memory plan),
- live stack frames (the T1Activation is zeroed and reused per
  invocation; partial deopt does not touch live activations),
- live GC maps (the `CompiledCode`'s `pc_map` and `deopt_points`
  are immutable post-publish; partial deopt builds a new
  `CompiledCode` for the new region).

---

## 8. Boundary ABI (v0 contract)

The region boundary ABI (Section 8 of the user's design) is the
contract between the partial-deopt-recompiled region and the
rest of the optimized graph (or the rest of the method's T1/T0
execution path). The B-2 v0:

```text
EntryState {
  control_token  : the entry control node's runtime value
                   (the T1Activation's status field; today
                   kStatusNormal)
  memory_state   : the entry memory node's runtime value
                   (the heap state at the entry FrameState's pc)
  live_values[]  : the entry values' runtime values
                   (interp::Value slots; the T1Activation's
                   slots[num_locals..num_locals+num_regs])
  exception_continuation : the exception exit's runtime value
                   (an interp::ObjRef; null if no pending
                   exception)
  deopt_state    : the FrameState at the entry control
                   (ir::FrameStateDesc; already in the IR)
}

ExitState {
  control_token  : the exit control node's runtime value
  memory_state   : the exit memory node's runtime value
  result_values[]: the exit values' runtime values
  exception_info? : the pending exception, if the exit is an
                   exception exit
}
```

The ABI prevents the rest of the optimized graph from needing
surgery: the partial-deopt-recompiled region produces the same
`ExitState` (modulo the corrected speculation) that the old
region would have produced, and the consumer (the next region
or the T1/T0 continuation) is unchanged.

---

## 9. Guard failure flow (v0 integration)

The guard-failure path is the integration point in
`compiler/codegen/src/Engine.cpp` `executeCompiled`. The v0 flow
(Section 9 of the user's design):

```text
on_guard_failure(CompiledCode* cc, T1Activation* act, DeoptId id):
  state = captureDeoptState(cc, act, id)  // existing FrameStateDesc path

  if not partial_deopt_enabled:
    return full_deopt_to_T0(state)  // existing path, Engine.cpp 900-934

  if not within_deopt_budget(cc->method_index):
    return full_deopt_to_T0(state)

  dirty = DependencyIndex::markFromGuard(cc, id)
  region = chooseMinimalSafeRegion(dirty)

  if region is unsafe (any check from Section 6 fails):
    return full_deopt_to_T0(state)

  // The currently executing thread moves to a safe fallback
  // IMMEDIATELY. Do NOT wait for recompilation while still
  // executing broken optimized code.
  run_fallback_stub(state)  // the T0 resume path

  schedule_partial_recompile(region)  // async in v1; sync in v0
```

Important: the v0 single-threaded case schedules the partial
recompile synchronously AFTER the T0 fallback has resumed. This
is correct (the fallback is the safety) but loses the perf win
(the next invocation of the same method re-triggers the partial
recompile). The v1 multi-threaded case (Section 5 of
`docs/STATUS.md`'s open-work list) is what unlocks the actual perf
salvage.

---

## 10. Fallback strategy (the escalation ladder)

The fallback ladder (Section 10 of the user's design) is, in
increasing severity:

1. **Side-exit to baseline/interpreter at the guard** — the T1
   baseline continues execution from the guard's RBC pc. Today
   the T1 baseline doesn't support mid-method side-exit (the
   T1 plan is a whole-method unit); this is a v1 feature.
2. **OSR exit if inside loop** — at the loop backedge or the
   next safepoint, exit to T0. Today's T0 fallback IS this path
   (the trap handler in `Engine.cpp` lines 900-934 builds a T0
   Frame and resumes in T0).
3. **Full method deopt** — what the v0 does today.
4. **Disable optimizations for method** — the method is
   blacklisted (Section 19); the next invocation goes straight
   to T1 baseline (no T2 install).
5. **Disable partial deopt globally** — if the failure rate is
   too high, the partial deopt layer is turned off (the feature
   flag `enable_partial_deopt` is set to false); the v0 method-
   granularity invalidation (`docs/deopt_backend.md` Section 17)
   remains.

The fallback must always exist. Production correctness depends on
it.

---

## 11. Partial rebuild algorithm (v0 work)

The partial rebuild (Section 11 of the user's design) is:

```text
partialRecompile(Region* oldRegion, DirtySet dirty):
  oldGraph = oldRegion->graph

  dirtyClosure = expandDirtyClosure(oldGraph, dirty)  // Section 5
  if dirtyClosure.size() > max_dirty_region_size:
    escalateFullRecompile(oldRegion)
    return

  frontier = computeBoundary(oldGraph, dirtyClosure)  // Section 6
  // frontier.inputs = entry values + entry memory
  // frontier.outputs = exit values + exit memory + exception exit

  // Rebuild from the RBC source (not the old graph; the old graph
  // is the speculation that broke). The new subgraph uses the
  // CURRENT type feedback + the CURRENT assumptions (which may
  // differ from the old region's assumptions — that's the point).
  newSubgraph = rebuildFromRBC(
      oldRegion->method,
      oldRegion->entryControl,  // the RBC pc to start rebuilding from
      currentTypeFeedback,
      currentAssumptions,
      frontier
  )

  // Insert new guards for any remaining speculation.
  insertGuards(newSubgraph, currentAssumptions)

  // Connect the new subgraph to the old region's frontier.
  connectInputs(frontier.inputs, newSubgraph)
  connectOutputs(newSubgraph, frontier.outputs)

  // Verify the new subgraph (Rule 40; the IR verifier).
  if not ir::verify(newSubgraph):
    discard(newSubgraph)
    escalateFullRecompile(oldRegion)
    return

  // Lower the new subgraph to machine code (codegen::lowerOnly,
  // already wired in the T2 driver).
  newCode = codegen::lowerOnly(newSubgraph, ...)
  if not newCode:
    escalateFullRecompile(oldRegion)
    return

  // Atomically activate (Section 17).
  activate(oldRegion, newCode)
```

The `rebuildFromRBC` step is the key new piece: the partial
rebuild does NOT reuse the old graph's node ids (the old
graph's speculation is what broke; rebuilding from the RBC
source with current feedback is the salvage). The new subgraph
is connected to the old region's frontier (the boundary ABI
from Section 8), and the old region's code is retired after no
threads are inside it.

---

## 12. What to preserve (the v0 invariant)

Unaffected optimized code is preserved (Section 12 of the user's
design). The v0 preserves:

- the outer method's compiled code (the T1 baseline's `CompiledCode`
  for the method, unchanged),
- unrelated optimized callees (other methods' T2-compiled code,
  unchanged),
- invariant hoisted code outside the dirty region (the dirty
  closure's complement in the graph),
- unchanged fast paths (the T1 baseline's stencil plan, unchanged),
- previously optimized non-dirty subgraphs (other regions in the
  same method, unchanged).

The v0 rebuilds:

- the failed guard's region (the dirty closure's minimal safe
  region),
- any inlined callee whose assumptions are in the dirty set
  (Section 16),
- any type-dependent loads/stores whose alias assumptions are in
  the dirty set (Section 14),
- any dependent phis / range checks / memory nodes whose inputs
  are in the dirty set.

---

## 13. Handling side effects (the hard part)

The side-effect rules (Section 13 of the user's design):

- Never duplicate a visible side effect.
- Never move a side effect across an incompatible memory
  barrier.
- Never drop a side effect unless it is provably dead.
- Never reorder effects unless memory edges allow it.
- If a dirty region contains calls/stores/allocations, be
  conservative.

The B-2 IR's `EffectKind` (from `b2/ir/Effect.h`) is the
classification: pure, read, write, call, alloc, barrier, etc.
The dirty-node closure (Section 5) uses `EffectKind` to decide
whether a node's semantics depend on the producer's value: a
pure arithmetic node's semantics depend on its operands' values;
a Call* node's semantics depend on its arguments' values AND on
the call's effect (which may include side effects the closure
must not duplicate).

If the side-effect closure is ambiguous (e.g., a Call* in the
dirty region whose effect chain forks into the region's exit
memory AND into another region's entry memory), the system
escalates to full method deopt. The v0 is conservative; the
v0 → v1 transition may relax the escalation criteria as the test
corpus grows.

---

## 14. Handling memory state (sea-of-nodes memory edges)

The sea-of-nodes memory edges (Section 14 of the user's design)
are the B-2 IR's `InputRole::Mem` (from `b2/ir/Node.h`). Every
memory-state node (Load*, Store*, Monitor*, New*, ClassInit,
Call*) has a `Mem` input that threads the memory state through
the graph.

For each region boundary:

```text
memory_in  = the Mem input of the region's first memory-state node
memory_out = the Mem input of the first memory-state node OUTSIDE
             the region (i.e., the region's last memory-state
             node's first user outside the region)
```

If dirty nodes include loads/stores:

- recompute alias dependencies (the alias class per memory node;
  the IR's `EffectKind` carries the coarse classification; the
  alias class is per-FieldId / per-Atype),
- rebuild memory phis if needed (a `Phi` with `InputRole::Mem`
  inputs; if the phi is in the region, it's rebuilt; if it's
  outside, the region is unsafe),
- preserve exception ordering (the `exceptionExit` boundary),
- preserve GC safe points (every FrameState in the region),
- preserve barrier semantics (`MemBar` nodes; the
  `MemBarKind` payload).

If the memory closure cannot be isolated (the memory-state
chain forks outside the region), the system escalates to full
method deopt.

---

## 15. Handling loops (OSR support required)

Loops are good partial recompilation targets (Section 15 of the
user's design), but require OSR (On-Stack-Replacement) support.
The B-2 v0 has:

- T0 → T1 OSR entry (the T1 baseline's safepoint_poll at loop
  heads, form (a) of `docs/baseline_contract.md` Rule 88; the
  corpus sweep is 19/19 today).
- T1 → T0 deopt (the trap handler in `Engine.cpp`).

The v0 does NOT have:

- T0 → T2 OSR entry (the T2 driver installs code at method entry
  only, not at loop backedges).
- T2 → T2 OSR (partial recompile of a loop region requires
  re-entering the optimized loop at the OSR entry point, which
  is a non-standard entry into the T2-compiled code).
- T2 → T0 OSR exit at loop backedge (the trap handler is at
  arbitrary RBC pc; the OSR exit at a loop backedge is a special
  case where the FrameState is the loop's LoopBegin FrameState).

The partial deopt loop case is open work; the v0 escalates to
full method deopt for any guard failure inside a loop.

---

## 16. Handling inlined code (the ICDG boundary)

Inlining is a common cause of partial deopt (Section 16 of the
user's design). The B-2 v0 has:

- the ICDG inline engine (`passes::runInlining`, the v1 inline
  pass; `docs/inlining.md` is the contract),
- the inline pass's per-site decisions (`InlineDecision` with
  `action ∈ {DirectInline, GuardInline, KeepIndirect}`),
- the FrameState caller chain (`FrameStateDesc.caller`).

The partial deopt boundary for inlined code is the inline site's
FrameState: the inlined callee is a sub-region whose entry
control is the call site's `Call*` node, whose exit control is
the call's continuation, and whose exception exit is the
`CallExcept` projection.

If an inlined callee's assumption breaks (e.g., a `TypeProfile`
guard fails because the receiver's class is no longer the
monomorphic profile), the dirty closure marks the inlined
sub-region dirty, the system rebuilds the caller region WITHOUT
that inline (or with a `GuardInline` → `KeepIndirect` downgrade),
and the unrelated optimized code in the caller is preserved.

If the inlining boundary is not clean (e.g., the inlined callee
escaped through a store to a heap field that's read outside the
inline site), the system escalates to full method deopt.

---

## 17. Activation must be atomic (epoch-based)

Atomic activation (Section 17 of the user's design) uses
epoch-based activation:

```text
newCode = compile(newRegion)
registerWithSafepointSystem(newCode)

at_safe_point:
  patch region entry to newCode
  retire oldCode after no threads inside
```

Requirements:

- old code remains valid until all threads exit (the v0
  single-threaded case: trivially true after the calling thread
  returns from `engine.run`),
- old GC maps remain valid (the v0 case: the old `CompiledCode`'s
  `pc_map` + `deopt_points` are immutable post-publish; they're
  kept alive until the `CompiledCode` is freed),
- no thread sees mixed old/new code (the v0 case: only the
  calling thread),
- patch points are limited and audited (the v0 case: the region
  entry is the only patch point; it's the `CodeEntry` in the
  `CompiledCode`),
- rollback is possible (the v0 case: if activation fails, the old
  region's code is still installed; the new region is discarded).

The v1 multi-threaded case needs the safepoint handshake
protocol (Rules 11, 13; `docs/deopt_backend.md` Section 17).
The v0 is synchronous — partial deopt runs in the calling
thread, after the T0 fallback has resumed.

---

## 18. Deopt state is mandatory (already in the IR)

Every Guard needs enough info to reconstruct a safe state
(Section 18 of the user's design). The B-2 IR already carries
this in `FrameStateDesc` + `SpecMeta`:

| User's `GuardDeoptState` field | B-2 IR field |
|---|---|
| `bytecode_location` | `FrameStateDesc.pc` (the RBC pc) |
| `interpreter_frame_layout` | the RBC method's `.regs N .locals M` directives |
| `baseline_frame_layout` | `CompiledCode.num_locals` + `num_regs` |
| `live_values` | the FrameState node's input edges (the local values) |
| `virtual_objects[]` | `FrameStateDesc.vobjOffset` + `vobjCount` (the PEA virtual objects to materialize) |
| `materialization_plan` | the `VirtualObjectState` nodes referenced by the vobj list |
| `stack_map` | `CompiledCode.pc_map` + `deopt_points` (the T1 baseline's stack map format, `docs/baseline_contract.md` SS4) |
| `exception_state` | the pending exception (the T1Activation's `pending_exc` field) |

For partial deopt, the FULL deopt state is still mandatory because
partial recompilation may fail (and then the system escalates to
full method deopt, which needs the full state).

---

## 19. Deopt budget and hysteresis (avoid deopt storms)

Deopt throttling (Section 19 of the user's design) tracks per
method/region:

```text
deopt_count
partial_recompile_count
failure_rate
time_since_last_deopt
compile_cost
```

Policy:

```text
if deopt_rate > threshold:
  disable partial deopt for method
  (the next invocation goes straight to T1 baseline)

if repeated_failure:
  downgrade method to baseline
  (the method's T2-compiled code is retired; the T1 baseline is
  the permanent tier for the method until the threshold resets)

if global_failure_rate > threshold:
  disable partial deopt globally
  (the feature flag `enable_partial_deopt` is set to false; the
  v0 method-granularity invalidation remains)
```

The thresholds are feature flags (Section 21). The v0 default is
conservative; the v0 → v1 transition tunes them based on
telemetry.

### v0.1 budget constants — single source of truth

The budget thresholds are NOT hard-coded magic numbers duplicated
across headers. The contract surfaces use a single source of truth:

- `kDefaultMaxDirtyRegionSize` (in `include/b2/pipeline/
  DirtyClosure.h`) — the default max IR nodes in the dirty closure
  before the algorithm escalates to full method deopt. The default
  value (64) has a documented rationale (at the constant's
  declaration): 64 IR nodes is roughly the size of a hot loop body
  or a small inlined callee, small enough that the partial rebuild
  is ~1-3 ms (the deopt-to-reopt latency budget, Section 22),
  large enough to cover the common salvage cases without trivially
  over-escalating on real Java methods.
- `PartialDeoptConfig::max_dirty_region_size` (in
  `include/b2/pipeline/PartialDeopt.h`) — the runtime config field,
  defaults to `kDefaultMaxDirtyRegionSize` (NOT to a duplicate
  literal). Override at runtime via this field; the algorithm
  (`expandDirtyClosure(g, seeds, max_size)`) takes the value as a
  parameter (no global state, Rule 125).
- `PartialDeoptConfig::partial_deopt_budget` — the per-method, per-
  window deopt count budget (default 3). The rationale is at the
  field's declaration: 1 deopt is normal, 2 is suspicious, 3 is the
  action threshold (the recompile is thrashing; stop attempting
  partial deopt for the method until the window resets).

The drift guard: `tests/pipeline/DirtyClosureTests.cpp`'s
`pipeline_dirty_closure_default_budget_single_source_of_truth` test
asserts `PartialDeoptConfig::max_dirty_region_size ==
kDefaultMaxDirtyRegionSize` (catches the case where someone
changes one without the other). The runtime-override test
`pipeline_dirty_closure_runtime_override_of_default_budget`
verifies the algorithm takes the parameter at the call site (no
hard-coded literal inside the algorithm).

The window size (`deopt_window_ms`) is NOT configurable at v0.1;
the v0 → v1 transition adds it based on telemetry.

---

## 20. Verification gates (Rule 40 form)

Verification before activation (Section 20 of the user's design)
runs the IR verifier (`ir::verify`, already implemented) on the
new subgraph. The v0 checks:

- type correctness (the IR verifier's operand-type check),
- control-flow reachability (every node is reachable from Start),
- region boundary consistency (the entry/exit control + values
  match the frontier),
- memory-state continuity (the entry memory's effect chain reaches
  the exit memory),
- effect ordering (the `EffectKind` chain is acyclic),
- phi consistency (every Phi's predecessors are reachable),
- exception edge validity (every Call* has its CallExcept
  projection wired),
- GC map validity (the `CompiledCode.pc_map` + `deopt_points`
  cover every Guard's DeoptId),
- stack map validity (the T1 baseline's stack map format),
- no duplicate side effects (Section 13),
- no dangling node references (every input is a live node),
- no cycles in data dependencies (the IR verifier's check),
- no illegal control cycles (the IR verifier's reducibility
  check).

In debug builds: verify always. In production: verify with
abort/escalate, or sampled verification if too expensive. If
verification fails, the system discards the new subgraph and
escalates to full method deopt.

---

## 21. Production safety policy (feature flags)

Partial deopt is opportunistic, not required for correctness
(Section 21 of the user's design):

```text
Correct path:
  interpreter/baseline always works (T0 / T1; the v0)

Optimized path:
  may partially recompile if safe (T2 + partial deopt)
  otherwise full deopt (the v0 path)
```

Feature flags (the v0 default is conservative; the defaults
documented at each field's declaration in
`include/b2/pipeline/PartialDeopt.h`, with rationales — see
Section 19 for the budget constants' single source of truth):

```text
enable_partial_deopt              = false (v0 default; opt-in)
enable_partial_loop_recompile     = false (requires OSR; Section 15)
enable_partial_inline_recompile   = false (requires ICDG boundary; Section 16)
allow_region_patching             = false (requires safepoint handshake; Section 17)
partial_deopt_budget              = 3 (per-method, per-window; rationale at field)
max_dirty_region_size             = kDefaultMaxDirtyRegionSize (64; rationale at
                                   the constant's declaration in DirtyClosure.h;
                                   PartialDeoptConfig defaults to the constant,
                                   not a duplicate literal)
verification_level                = "always" (debug) / "sampled" (release)
```

Shadow mode (compute partial plan, verify it, do not activate,
compare with full deopt behavior) is the v0 → v1 transition's
validation tool. The v0 default is `enable_partial_deopt = false`
(everything shadows); the v1 default flips to `true` once the
shadow comparison passes the corpus.

---

## 22. Telemetry

Telemetry (Section 22 of the user's design) tracks:

```text
guard_failure_reason       // the GuardKind + the DeoptId
invalidated_dependency_key  // the DependencyId
selected_region            // the RegionId
dirty_node_count           // the dirty closure size
region_expansion_reason    // why the closure expanded (forward / backward)
verification_failures      // the IR verifier's diagnostics
fallback_reason            // "unsafe region" / "budget exceeded" / "verify failed"
partial_recompile_success_rate
deopt-to-reopt latency     // ms from guard failure to new code activation
code_patch_failures        // the atomic swap path
deopt_loops                // repeated deopt at the same site
method_blacklisting        // the throttling policy's actions
memory/state mismatches    // the boundary ABI's checks
```

If a method repeatedly triggers partial failures, it's
blacklisted (Section 19). The telemetry surface is the T1
engine's existing `Tier1Stats` (extended with partial-deopt
counters); the `b2t2 --stats` output (mirrors `b2jit --stats`)
reports them.

---

## 23. Dangerous cases to reject immediately

Do not attempt partial rebuild if (Section 23 of the user's
design):

- dirty region crosses exception handler boundaries badly,
- dirty region contains uncontrolled side effects (Call* to
  unknown methods; the alias analysis is unbounded),
- memory closure is unclear (the memory-state chain forks
  outside the region in a way the effect classification can't
  resolve),
- phis cannot be reconstructed (the phi's predecessors are
  outside the region and not in the entry values),
- GC maps would change unpredictably (the new region's GC
  reference liveness differs from the old region's),
- stack frames are already materialized inconsistently (the
  T1Activation's monitor stack is non-empty; the partial
  rebuild would need to reproduce the monitor state),
- code patching is required inside non-patchable machine code
  (the JIT arena's W^X discipline forbids in-place patching;
  the v0 builds a new region instead),
- region size exceeds threshold (`max_dirty_region_size`),
- deopt budget exceeded (`partial_deopt_budget`),
- verifier has any doubt (the IR verifier fails; the
  verification gates from Section 20).

Escalate to full method deopt in all these cases.

---

## 24. Recommended architecture

The architecture (Section 24 of the user's design):

```text
                   Runtime Event
                        |
                 DependencyIndex (Section 2; v0 work)
                        |
                  Dirty Nodes (Section 5; v0 algorithm)
                        |
              Minimal Safe Region (Section 6; v0 algorithm)
                        |
                  Region Builder (Section 11; v0 work)
                        |
                  Graph Verifier (Section 20; already implemented)
                        |
                 Partial Compiler (codegen::lowerOnly; already wired)
                        |
                 Code Activation (Section 17; v0 synchronous)
                        |
                 Epoch Reclamation (Section 7; v0 trivial)
```

With fallback:

```text
Any failure -> full deopt to T0 (the existing path in Engine.cpp)
```

The v0 residents of `compiler/pipeline/` (currently a stub
directory, `MSG-20260918-004`):

- `DependencyIndex.h` — the runtime inverse map
  (`DependencyId → set<NodeId> / set<RegionId>`).
- `Region.h` — the recompilation region structure
  (Section 4's boundary contract).
- `PartialDeopt.h` — the guard-failure integration surface
  (Section 9's flow).
- `DirtyClosure.h` — the dirty-node closure algorithm
  (Section 5).
- `RegionSafety.h` — the safety checks (Section 6).
- `PartialRecompile.h` — the partial rebuild (Section 11).
- `Activation.h` — the epoch-based atomic swap (Section 17).
- `Telemetry.h` — the partial-deopt counters (Section 22).

All v0 residents are headers-only (no .cpp files); the
implementation is the v0 → v1 transition's work.

---

## 25. Final design principle

For production (Section 25 of the user's design):

```text
Partial deopt must never be required for correctness.
It is only a performance salvage mechanism.
```

The B-2 rule:

```text
If we can prove partial rebuild is safe, do it.
If we cannot prove it, deopt fully to T0.
```

The v0 default is `enable_partial_deopt = false`; partial deopt is
shadow-only (compute the plan, verify it, do not activate). The
v1 default flips to `true` once the shadow comparison passes the
corpus. Until then, the v0 method-granularity invalidation
(`docs/deopt_backend.md` Section 17) and the T0 fallback
(`compiler/codegen/src/Engine.cpp` `executeCompiled` lines 900-934)
are the production path.

---

## See also

- `docs/deopt_backend.md` — the v0 deopt system design (Part A) +
  the v0 backend design (Part B). Section 17 is the
  method-granularity invalidation this contract extends.
- `docs/t2_driver_contract.md` — the T2 driver (the consumer of
  partial deopt; without it, every T2 guard failure is a full
  method deopt).
- `docs/codegen_contract.md` — the codegen team contract (the
  guard-failure integration point).
- `docs/pass_contracts.md` — the passes team contract (the
  pipeline orchestrator hosting the DependencyIndex + Region
  structures).
- `docs/jit_hardening.md` — the JIT hardening v0 (W^X) that
  partial deopt must respect (no in-place patching of live
  machine code).
- `docs/baseline_contract.md` SS4 — the T1 baseline's stack map
  format (the GC-reference liveness contract surface for partial
  deopt's region boundaries).
- `include/b2/ir/Graph.h` — the IR's `Dependency` +
  `SpecMeta.dependency` + `FrameStateDesc` scaffolding (already
  exists).
- `include/b2/ir/Node.h` — the IR's `NodeKind::Guard` +
  `GuardKind` + `NodeFlag::Speculative` (already exists).
- `compiler/codegen/src/Engine.cpp` `executeCompiled` lines
  900-934 — the guard-failure path (the integration point).
- `messages/open/MSG-20260918-007-...INFO.md` — this contract's
  announcement.
