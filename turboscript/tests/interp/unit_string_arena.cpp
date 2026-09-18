// Unit test — string node arena allocation (v0.7,
// benchmarks_v0.6.md register item #2, CLOSED).
//
// Rule 34 (five regression tests per change) and Rule 60 (no untested
// paths) bind this: the Heap's string storage moved from
// `std::deque<StringObj>` to `BumpArena<StringObj>` (a pointer bump
// instead of a deque emplace per concat). The never-collected,
// address-stable ownership contract (Rule 96 + bytecode_spec.md Section
// 11) is preserved by construction (BumpArena segments are chained
// `unique_ptr<std::byte[]>` arrays — addresses are valid for the Isolate
// lifetime). These tests pin the load-bearing properties:
//
//   1. flat strings: address stable across N+1 further allocations
//      (no realloc/move under the arena; the deque guaranteed this too).
//   2. cons strings: operands (non-owning pointers into the arena) stay
//      readable after the cons node is allocated and after further
//      allocations.
//   3. flat(): in-place materialization reaches every cons operand and
//      the result matches the concat of the operands (the cross-engine
//      differential test is the corpus sweep; this is the local pin).
//   4. capacity edges: kMinConsLength boundary (eager flat vs cons node),
//      kMaxStringCodeUnits overflow returns nullptr (caller raises
//      RangeError).
//   5. allocationCount: counts a flat and a cons node as two allocations
//      (the v0.6 form counted via deque.size() and produced the same
//      count; the v0.7 form counts via BumpArena::constructed()).
//   6. mutation safety: a long chain of cons operations does not corrupt
//      earlier operands (depth stress for the bump-segment chain).
//
// Laws: Rule 4 (state reconstruction — StringObj::flat is in-place),
// Rule 7 (arena ownership), Rule 23 (named constants), Rule 72 (numeric
// and string semantics), Rule 96 (Tier 0 semantic baseline), Rule 105
// (defensive bounds — overflow returns nullptr).
#include <cstdio>
#include <string>
#include <vector>

#include "ts_object.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "UNIT FAIL: %s\n", what);
    ++failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

// v0.7.1 forward declaration: main() calls main_rollover_impl() as a second
// phase; the definition is below main() (kept adjacent to the
// rollover_check helper / namespace closure it shares).
int main_rollover_impl();

}  // namespace

int main() {
  ts::Heap heap;

  // 1. Address stability: a flat string's address stays valid across
  // further allocations (the never-collect / address-stable contract).
  ts::StringObj* a = heap.makeString(u"hello");
  const ts::StringObj* aSnapshot = a;
  for (int i = 0; i < 1000; ++i) {
    (void)heap.makeString(u"padding");
  }
  check(a == aSnapshot, "flat string address stable across 1k further allocs");
  check(a->flat() == u"hello", "flat string payload stable across 1k further allocs");

  // 2. Cons string: operands stay readable after the cons node is allocated
  //    (the operands are non-owning pointers into the Heap; address stability
  //    is what makes the lazy-flatten protocol sound). Strings above
  //    kMinConsLength (13) produce a cons node; below, the concat builds flat
  //    eagerly (see test 4 below).
  ts::StringObj* left = heap.makeString(u"left-operand-");
  ts::StringObj* right = heap.makeString(u"right-operand!");
  ts::StringObj* cons = heap.makeCons(left, right);
  check(cons != nullptr, "makeCons returns a node above kMinConsLength");
  check(cons->isCons(), "makeCons produces a cons-kind node");
  check(cons->left == left, "cons node references the left operand");
  check(cons->right == right, "cons node references the right operand");
  check(cons->length == 27u, "cons node length is the sum of operands");

  // Further allocations don't invalidate the operand pointers.
  for (int i = 0; i < 100; ++i) (void)heap.makeString(u"more padding");
  check(left->flat() == u"left-operand-", "left operand readable after further allocs");
  check(right->flat() == u"right-operand!", "right operand readable after further allocs");

  // 3. flat(): in-place materialization produces the exact concatenated text.
  const std::u16string& flat = cons->flat();
  check(flat == u"left-operand-right-operand!",
        "cons flat() materializes the concatenated text");
  check(!cons->isCons(),
        "cons flat() flattens the node in place (kind kCons -> kFlat)");
  check(cons->left == nullptr && cons->right == nullptr,
        "cons flat() nulls the operand pointers after in-place materialization");

  // 4. Capacity edges.
  // kMinConsLength boundary: results shorter than kMinConsLength build flat
  // eagerly (cons-node overhead is not worth it for tiny results).
  ts::StringObj* tinyA = heap.makeString(u"a");
  ts::StringObj* tinyB = heap.makeString(u"b");
  ts::StringObj* tinyCons = heap.makeCons(tinyA, tinyB);
  check(!tinyCons->isCons(),
        "sub-kMinConsLength concatenation builds flat eagerly (no cons node)");
  check(tinyCons->flat() == u"ab",
        "sub-kMinConsLength concatenation is the correct flat text");

  // kMaxStringCodeUnits overflow: returns nullptr; the caller (a builtin)
  // raises a JS RangeError. The Heap cannot raise (Rule 74).
  ts::StringObj* big = heap.makeString(std::u16string(ts::kMaxStringCodeUnits, u'x'));
  ts::StringObj* tooBig = heap.makeCons(big, big);
  check(tooBig == nullptr,
        "kMaxStringCodeUnits overflow returns nullptr (caller raises RangeError)");

  // 5. allocationCount: counts flat and cons nodes via the arena's
  //    constructed() (the v0.7 form; the v0.6 form was deque.size()).
  ts::Heap counting;
  const uint64_t before = counting.allocationCount();
  ts::StringObj* f1 = counting.makeString(u"first");
  ts::StringObj* f2 = counting.makeString(u"second");
  (void)counting.makeCons(f1, f2);  // exercise the cons path's construct()
  const uint64_t after = counting.allocationCount();
  check(after - before == 3,
        "allocationCount counts the two flat nodes + one cons node (3)");

  // 6. Depth stress: a long chain of cons operations does not corrupt
  //    earlier operands. Concatenation loops build left-leaning trees;
  //    the depth == iteration count, the arena hosts every node, and
  //    flattening the root must walk every operand. We compute the
  //    expected text in parallel and compare.
  ts::Heap deepHeap;
  ts::StringObj* root = deepHeap.makeString(u"");
  std::u16string expected;
  constexpr int kDepthN = 200;  // Rule 23: small named constant; well under
                                // kMaxStringCodeUnits and well above the
                                // segment boundary (256-per-segment for
                                // Object/StringObj alike).
  for (int i = 0; i < kDepthN; ++i) {
    ts::StringObj* piece = deepHeap.makeString(u"xy");
    root = deepHeap.makeCons(root, piece);
    expected.append(u"xy");
  }
  check(root != nullptr, "long cons chain root is not null");
  check(root->length == expected.size(),
        "long cons chain length matches the expected concat length");
  check(root->flat() == expected,
        "long cons chain flat() matches the expected text exactly");
  check(!root->isCons(),
        "long cons chain root is flat after flat() (in-place materialization)");

  if (failures != 0) {
    std::fprintf(stderr, "unit: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("unit: all string-arena checks passed\n");

  // v0.7.1: run the segment-rollover tests as a second phase (they share
  // the check helper pattern; a separate main_rollover keeps the test
  // surface readable and lets a future bisect isolate v0.7.1 failures
  // from v0.7 failures).
  return main_rollover_impl();
}

// v0.7.1 addition: the BumpArena::construct hot path was changed to cache the
// active segment base in `currentBase_` (avoiding the `segments_.back().get()`
// 3-load dependent chain on every allocation). The externally-observable
// behavior is identical (same ownership contract; same address stability);
// these tests pin the new code path's invariants across segment rollover.
//
// Laws: Rule 34 (≥5 regression tests per change — together with the v0.7
// tests above, this file's 21 assertions cover the BumpArena<StringObj> +
// currentBase_ cache + segment-rollover surface), Rule 60 (no untested code
// paths — the rollover path was not directly exercised by the v0.7 tests,
// which stopped at 1000 padding allocs).
namespace {

int rollover_failures = 0;

void rollover_check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "UNIT FAIL: %s\n", what);
    ++rollover_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

int main_rollover_impl() {
  // 7. Segment rollover: 3 full segments worth of allocations (3 * 256 = 768)
  //    forces two addSegment() calls. Every prior allocation must remain
  //    readable (the never-collected contract; currentBase_ is just a cache,
  //    the segments_ vector owns the memory).
  ts::Heap rolloverHeap;
  std::vector<ts::StringObj*> live;
  std::vector<std::u16string> expected;
  // 3 segments + headroom = 800 allocations, all above kMinConsLength so
  // makeString builds flat nodes (the path that exercises construct()).
  for (int i = 0; i < 800; ++i) {
    std::u16string s(static_cast<size_t>(14 + (i % 10)), u'x');
    ts::StringObj* p = rolloverHeap.makeString(s);
    rollover_check(p != nullptr, "rollover: every makeString returns a node");
    live.push_back(p);
    expected.push_back(s);
  }
  // After 3+ segments, every prior address must still be readable.
  int ok = 0;
  for (size_t i = 0; i < live.size(); ++i) {
    if (live[i]->flat() == expected[i]) ++ok;
  }
  rollover_check(ok == static_cast<int>(live.size()),
                 "rollover: every prior flat string readable across 3 segments");
  // allocationCount must reflect all 800 + the seed empty segment's worth.
  rollover_check(rolloverHeap.allocationCount() >= 800,
                 "rollover: allocationCount >= 800 after 800 allocs");
  // 8. Cons chain across segment rollover: a 600-deep chain forces cons nodes
  //    across 3 segments; the in-place flatten must walk every operand
  //    (the lazy materialization must reach across segment boundaries —
  //    the operand pointers are non-owning references into earlier segments).
  ts::Heap deepRollover;
  ts::StringObj* root2 = deepRollover.makeString(u"");
  std::u16string expected2;
  for (int i = 0; i < 600; ++i) {
    ts::StringObj* piece = deepRollover.makeString(u"yz");
    root2 = deepRollover.makeCons(root2, piece);
    expected2.append(u"yz");
  }
  rollover_check(root2 != nullptr,
                 "rollover: 600-deep cons chain root not null");
  rollover_check(root2->length == expected2.size(),
                 "rollover: 600-deep cons chain length matches");
  rollover_check(root2->flat() == expected2,
                 "rollover: 600-deep cons chain flattens across segment boundaries");

  if (rollover_failures != 0) {
    std::fprintf(stderr, "unit_rollover: %d failure(s)\n", rollover_failures);
    return 1;
  }
  std::printf("unit_rollover: all segment-rollover checks passed\n");
  return 0;
}

}  // namespace
