# B-2 RegAlloc tests: v0 stub directory.
#
# No tests live here today. The regalloc team's charter
# (docs/teams/regalloc-team.md) describes what will be tested when v1
# lands; until then this directory exists only so
# docs/teams/ownership.yaml is consistent with the tree.
#
# When v1 lands, the test harness will follow the tests/rbc/CMakeLists.txt
# pattern: a b2_regalloc_tests executable that links b2::regalloc, an
# add_test() entry, and a corpus/ subdirectory of fixtures.

# Intentionally no add_test() call today.
