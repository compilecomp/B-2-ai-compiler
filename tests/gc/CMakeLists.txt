# B-2 GC tests: v0 stub directory.
#
# No tests live here today. The gc team's charter (docs/teams/gc-team.md)
# describes what will be tested when v1 lands; until then this directory
# exists only so docs/teams/ownership.yaml is consistent with the tree.
#
# When v1 lands, the test harness will follow the tests/rbc/CMakeLists.txt
# pattern: a b2_gc_tests executable that links b2::gc, an add_test() entry,
# and a corpus/ subdirectory of fixtures.

# Intentionally no add_test() call today.
