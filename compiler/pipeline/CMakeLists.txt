# B-2 Pipeline: v0 stub directory (passes team).
#
# WHY THIS IS A STUB:
# docs/teams/ownership.yaml lists compiler/pipeline/ as a passes-team
# write path (the comment there reads "Special pass designs ..."). The
# actual pass bodies live in compiler/passes/ today; this directory is
# the forward home for the T2 execution driver and the pipeline
# orchestrator that orders passes, manages budgets, and lowers IR to
# machine code (or to T1 stencil plans).
#
# The hard dependency is the T2 execution driver itself; see
# docs/STATUS.md's "Recommended next steps" item 4. Until the driver
# lands, no pipeline orchestrator can run.
#
# This CMakeLists.txt declares an INTERFACE library so the directory
# is consistent with the ownership map and the v0 -> v1 transition can
# add real source files here without touching any consumer's build.

add_library(b2_pipeline INTERFACE)
add_library(b2::pipeline ALIAS b2_pipeline)
target_include_directories(b2_pipeline INTERFACE
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)
