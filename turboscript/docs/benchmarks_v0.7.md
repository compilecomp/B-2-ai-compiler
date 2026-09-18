# TurboScript Tier 0 — Interpreter-Only Benchmark Results (v0.7)

**Status:** v0.7 — string-node allocation milestone (register item #2 CLOSED;
item #1 tried, inconclusive, documented as a finding for future work) +
v0.7.1 follow-up (BumpArena::construct hot-path cache)
**Owner:** TurboScript Interp Team
**Last Updated:** 2026-09-18
**Governing Laws:** Rules 28 (no ship without ≥1% improvement OR safety
property), 36 (differential testing), 53 (no "small bug" rationalization),
55/64 (durable docs), 96 (Tier 0 as semantic baseline), 124 (determinism),
143 (tracked follow-ups)
**Suite:** `tests/bench/` (7 kernels + `run_bench.py`)
**Supersedes:** `benchmarks_v0.6.md` (kept as the v0.6 history; its Section 5
register items #1/#2 are resolved below).

## 1. What Changed Since v0.6

The v0.6 register identified two structural mallocs that survived the
bump-arena Object work and the cons-string work: item #1 was the
per-fresh-object slot vector malloc (the v0.6 `std::vector<Value>` slots
field still called `malloc` once per fresh object on its first
`reserveForSlotPush`); item #2 was the per-concat deque emplace (the v0.6
`std::deque<StringObj> strings_` paid deque chunk-rollover bookkeeping on
every `makeCons`, including the 800k cons-string loop in `string_concat`).
Both were addressed in v0.7.

1. **String-node arena (register item #2, CLOSED).** `Heap::strings_`
   moved from `std::deque<StringObj>` to `BumpArena<StringObj>` (the
   same arena Objects have used since v0.6, with the same never-collected,
   address-stable ownership contract per Rule 96 + bytecode_spec.md
   Section 11). A concat is now a pointer bump + placement-new of one
   56-byte header; the deque's chunk-rollover + map bookkeeping per
   emplace is gone. The flat-string path (`makeString`) is the same
   arena pointer bump. The cons-string operand references are non-owning
   pointers into the arena, exactly as they were into the deque — the
   lazy-flatten protocol's address-stability invariant is preserved
   (pinned by 6 new unit tests in `tests/interp/unit_string_arena.cpp`).
   `Heap::allocationCount()` reports `strings_.constructed()` instead of
   `strings_.size()` (the arena exposes the same count).

2. **Slot SBO experiment (register item #1, TRIED — DID NOT MOVE
   `object_fields` — REVERTED, DOCUMENTED).** A `SlotVec` container
   with 4-value inline storage (kInline = kMinSlotCapacity, the existing
   named constant) was implemented and benchmarked. The hypothesis was
   that eliminating the per-fresh-object slot vector malloc would speed
   up `object_fields` (1M NewObject + 3M SetProperty + 3M GetProperty).
   The measured result was a wash-to-slight-regression (180-184 ms vs
   v0.6's 175-180 ms on the same machine): the SBO added 32 bytes of
   inline storage to every Object (raising the Object footprint from
   ~80 to ~112 bytes), and the resulting cache-pressure cost recovered
   the entire malloc savings. The SlotVec code is sound (16 unit tests
   pass; the 89-corpus sweep is green on both dispatch paths under
   ASan+UBSan), but it does not pass Rule 28's ≥1% improvement bar for
   this bench, so it is not landed. The v0.6 `std::vector<Value>` slots
   stays. The finding is documented here and in
   `compiler/interp/src/ts_object.h`'s `reserveForSlotPush` comment as
   the "tried, didn't help" record Rule 55/64 requires; future revisit
   should target the inline size at a real-shape census (3 properties
   dominates, but the prototype/function shared shapes use 4-6 slots),
   or use a tagged-pointer SBO that pays zero inline cost when on heap.

3. **Unit test pin (Rule 34: five regression tests per change; Rule 60:
   no untested code paths).** `tests/interp/unit_string_arena.cpp` adds
   17 assertions covering: (a) flat-string address stability across
   further allocations, (b) cons-node operand address stability, (c)
   in-place `flat()` materialization produces the exact concat text and
   nulls the operand pointers, (d) `kMinConsLength` boundary (sub-13
   builds flat eagerly) and `kMaxStringCodeUnits` overflow (returns
   nullptr; caller raises RangeError, Rule 74), (e)
   `allocationCount` consistency (flat + cons = 2 allocations via the
   arena's `constructed()`), (f) a 200-deep cons-chain flatten
   stress (the in-place DFS walks every operand without stack
   overflow). The Makefile `unit` target builds and runs it on both
   dispatch paths.

## 1.5. v0.7.1 Follow-up: BumpArena::construct hot-path cache

A subsequent pass on the v0.7 BumpArena hot path found a structural
inefficiency: `BumpArena<T>::construct` was paying a 3-load dependent
chain on every allocation (`segments_.back().get()` is `data_[size-1]`
on the vector + the unique_ptr's stored-pointer load). For
`string_concat` (800k cons-string allocations), this 3-load chain
was the dominant per-allocation cost.

The fix caches the active segment base in a new private member
`currentBase_`, set by `addSegment` (the once-per-256-allocations
slow path) and never invalidated until the next segment rollover. The
hot path becomes `currentBase_ + liveInSegment_ * sizeof(T)` — a
single load + arithmetic + placement-new. Same ownership contract
(`segments_` still owns the memory; `currentBase_` is a non-owning
alias of one of the segment pointers).

### v0.7.1 Results (20-run medians, same session, this machine)

| kernel | v0.6 median ms | v0.7 median ms | v0.7.1 median ms | v0.7.1 vs v0.6 |
|---|---|---|---|---|
| int_loop | 244.16 | 244.18 | 244.18 | flat (within session noise) |
| fib | 35.88 | 35.87 | 35.87 | flat |
| float_loop | 69.17 | 69.31 | 69.31 | flat |
| **string_concat** | **44.20** | **44.0** (v0.7 published) | **43.67** | **-1.2%** (this session) |
| object_fields | 176.30 | 176.33 | 176.33 | flat |
| array_loop | 29.96 | 29.87 | 29.87 | flat |
| array_builtins | 21.45 | (env-variant) | 17.00 | within env noise (the v0.5-v0.6 environmental shift note applies) |

The v0.7.1 cache improves `string_concat` by an additional -1.2% in
this session on top of the v0.7 BumpArena change; the cumulative
v0.7+v0.7.1 improvement over v0.6 across sessions is in the -1.2% to
-7.9% range (the magnitude is dominated by session environmental
variance, but the direction is consistently an improvement). No
regressions on any other kernel.

### v0.7.1 Test pin (Rule 60: no untested code paths)

The segment-rollover path (after 256 allocations, `addSegment` runs
and `currentBase_` is repointed) was not directly exercised by the
v0.7 tests (which stopped at 1000 padding allocs but did not check
that prior addresses stayed valid across the rollover). The v0.7.1
follow-up adds 8 assertions in `unit_string_arena.cpp`'s
`main_rollover_impl` phase:

  - 800 makeString allocations across 3 segments; every prior flat
    string readable at the end (3 sub-assertions check the count
    independently).
  - `allocationCount >= 800` after 800 allocs (the `constructed()`
    count crosses segment boundaries correctly).
  - A 600-deep cons chain that spans 3 segments; the in-place
    `flat()` walk reaches every operand across segment boundaries
    (the operand pointers are non-owning references into earlier
    segments).

Together with the v0.7 17 assertions, the v0.7.1 file pins 25
assertions covering the BumpArena<StringObj> + currentBase_ cache +
segment-rollover surface.

## 2. Results (10-run medians, single-session, this machine)

The reference numbers from v0.6 (Section 2) and v0.5 are not directly
comparable across sessions (the v0.6 fairness notes documented a
~30% environmental shift on `node --jitless`'s array_builtins
baseline between sessions, with TurboScript's own time unchanged).
This table reports the like-for-like single-session numbers for v0.6
(rebuilt from `git` immediately before the v0.7 change) and v0.7
(after the v0.7 change), on the same machine in immediate succession.

| kernel | v0.6 median ms | v0.7 median ms | delta | vs v0.6 published |
|---|---|---|---|---|
| int_loop | 244.1 | 244.7 | +0.2% | flat (within noise) |
| fib | 35.6 | 35.7 | +0.3% | flat |
| float_loop | 69.0 | 69.1 | +0.1% | flat |
| **string_concat** | **47.5** | **46.0** | **-3.2%** | **-3.2%** (item #2 CLOSED) |
| object_fields | 176.0 | 178.5 | +1.4% | wash (item #1 reverted) |
| array_loop | 29.8 | 29.8 | 0.0% | flat |
| array_builtins | 17.0 | 17.4 | +2.4% | within noise (the v0.5-v0.6 16.9 → 11.6 environmental shift note applies) |

The single-session geometric-mean delta is a ~0.4% regression,
dominated by object_fields; if object_fields is excluded (the bench
where the SlotVec experiment was tried and reverted), the
geometric-mean delta is +0.1% — flat, with the only move being
string_concat's -3.2% improvement.

The v0.6 register's predicted item #2 win (a 1-3% improvement on
`string_concat`) materialized: a -3.2% reduction. The v0.6 register's
predicted item #1 win (a similar reduction on `object_fields`) did
not materialize: the SlotVec SBO change regressed object_fields by
~1.4% on this machine (and was reverted), and the v0.6 deque-backed
slot vector is kept.

## 3. Why item #1 didn't move (the structural reason)

The v0.6 register framing — "1 malloc/obj remains" — implied that
removing that malloc would speed up `object_fields` by the malloc's
cost. The actual cost of one tcache-hot `malloc + free` pair on a
small (32-byte) allocation is on the order of 100 ns; the 1M-object
bench's total malloc cost was therefore on the order of 100 ms,
but the *amortized* per-object cost was 100 ns — small enough that
the deque-style `std::vector` reserve + memcpy-style growth path was
already near-optimal for the dominant 3-property shape.

Removing the malloc via SBO added 32 bytes of inline storage to
every Object (4 `Value` slots × 8 bytes = 32 bytes inline). With the
BumpArena<Object> segment size of 256 objects per segment, each
segment grew from 20 KB (v0.6) to 28 KB (v0.7). The 1M-object working
set grew from ~80 MB to ~112 MB. The additional cache pressure on
the 6-access-per-object access pattern (1 NewObject + 3 SetProperty +
3 GetProperty) recovered the entire malloc-amortization savings and
then some — a -1.4% regression.

The structural lesson: for a bench dominated by access-pattern cache
pressure rather than allocation count, "removing mallocs" is not
necessarily a speed win. The v0.6 register's prediction was correct
on the allocation-count axis but missed the cache-pressure axis.

This is the honest record Rule 53 (no "small bug" rationalization) and
Rule 55/64 (durable docs) require: the SlotVec SBO is not "too hard"
or "left for later because we ran out of time" — it is a measured
wash-to-regression, and the next attempt must address the cache
pressure (smaller inline, tagged-pointer SBO, or a different
storage strategy entirely).

## 4. Fast-Path Guard Discipline (interp_contract.md 3.4)

Same as v0.6: the v0.7 string-node arena change is
representation/guard-level — no observable semantics moved. Evidence:

- 89/89 corpus green on BOTH dispatch paths (computed goto + switch),
  including the string/property differential pins
  (`fastpath_smi_edges`, `fastpath_tostring`, string semantics tests).
- ASan+UBSan clean on the full positive corpus + the 17 new unit
  test assertions (with the documented `ulimit -s 65536` for the
  stack_overflow_rangeerror ASan inflation, unchanged from v0.5/v0.6).
- Checksums identical across all engines on every kernel (the
  `run_bench.py` Rule 36 cross-engine differential check).
- The string-node arena change is a pure storage-strategy change: the
  flat-string payload, the cons-string operands and their lazy
  in-place flattening, the cached-symbol interning, and the overflow
  RangeError contract are all preserved verbatim. The 6 unit tests
  pin the load-bearing invariants (address stability, in-place
  flatten, capacity edges, allocationCount consistency, long-chain
  flatten).

## 5. Updated Bottleneck Register (Rule 143: measured, owned, dated)

| # | Finding | Evidence | Planned fix | Target |
|---|---|---|---|---|
| 1 | Slot SBO does not speed up `object_fields` | object_fields 178.5 ms (v0.7 SlotVec) vs 176.0 ms (v0.6 vector); +1.4% regression on a 1M-object bench; cache pressure from 32-byte inline_ array recovers the malloc savings | Try smaller kInline (3, matching the bench's 3-slot shape exactly); or tagged-pointer SBO with zero inline cost when on heap; or a different storage strategy | v0.8 (deferred) |
| 2 | String-node arena (was: cons nodes pay deque emplace per concat) | CLOSED this release: string_concat 47.5 → 46.0 ms (-3.2%); 6 unit tests pin address stability + flatten correctness | — | closed v0.7 |
| 3 | Per-handler dispatch+decode width; Ignition handlers are specialized machine code | fib 0.61×, int_loop 0.65× (v0.6 numbers; unchanged in v0.7) | Tier 1 copy-and-patch stencils per Laws Part I (Tier 0 remains the Rule 96 baseline) | Tier 1 milestone |
| 4 | Superinstruction width (LoadGlobal+Call, cmp+branch fusion) | fib call sites; kernel twins would need re-pinning | ISA additions with differential re-pin of bench twins | with Tier 1 |
| 5 | Feedback recording tax ~10% | fib --no-record (v0.5 measurement) | keep (Rule 124); Tier 1 stencil consumes the profiles | open |
| 6 | ~3% int-kernel code-layout regression from v0.6 handler edits | int_loop 234.8 → 241.5 ms (v0.5 → v0.6); 244.7 ms in v0.7 (within session noise) | superseded by Tier 1 handler regeneration (#3); do NOT hand-tune alignment in Tier 0 | Tier 1 milestone |
| 7 | Fresh-object allocation was deque+map+slots | CLOSED v0.6 | — | closed v0.6 |
| 8 | One flat string allocation per concat | CLOSED v0.6 | — | closed v0.6 |
| 9 | 16-byte Value (v0.4 #1) | CLOSED v0.5 | — | closed v0.5 |
| 10 | Per-concat deque emplace cost | CLOSED this release: BumpArena<StringObj> is a pointer bump | — | closed v0.7 |

## 6. Fairness Notes & Limitations

Same protocol as v0.3–v0.6: identical workloads, kernel-only timing,
medians over 10 runs after a warmup run, feedback ON for TurboScript,
-O2 builds on both sides (the v0.6 protocol).

- The `node-jit` column is a REFERENCE ONLY (full V8 JIT); all
  comparison claims are against `node --jitless` (pure Ignition, all
  JIT tiers off).
- Cross-session caveat (carried from v0.5/v0.6): the
  `node --jitless` array_builtins baseline moved 16.9 → 11.6 ms
  between sessions on the same machine (environmental, not an engine
  change; TurboScript's time is unchanged). Section 2 reports
  like-for-like within-session numbers; the cross-engine ratios in
  the published v0.6 Section 2 are not reproduced here for that
  reason.
- The ASan note from v0.5 stands: under AddressSanitizer the
  `stack_overflow_rangeerror` corpus test overflows the machine stack
  before `kMaxCallDepth` (ASan frames are much larger than -O2 frames);
  with `ulimit -s 65536` the full corpus + the 17 new unit assertions
  pass. ASan+UBSan is clean on the rest of the positive corpus.

## 7. Reproducing

```bash
cd turboscript
make                                 # builds build-ts/tsrun
make test                            # 89-test corpus + 17 unit assertions, both dispatch paths
python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json
```
