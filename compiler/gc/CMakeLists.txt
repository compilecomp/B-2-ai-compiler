# B-2 GC: v0 stub directory.
#
# WHY THIS IS A STUB:
# docs/teams/ownership.yaml lists compiler/gc/, include/b2/gc/, and
# tests/gc/ as the gc team's write paths. docs/gc.md describes a
# "Generational Concurrent Region-Based Collector". No code exists today.
# See docs/STATUS.md ("What does NOT exist") and docs/gc.md for the design.
#
# This CMakeLists.txt declares an INTERFACE library so:
#   - the directory is consistent with the ownership map,
#   - consumers can link b2::gc (today a no-op),
#   - the v0 -> v1 transition can add real source files here without
#     touching any consumer's build.
#
# DO NOT add source files here without landing docs/gc.md's v1 contract
# and an INFO message to the integrator-owned message space.

add_library(b2_gc INTERFACE)
add_library(b2::gc ALIAS b2_gc)
target_include_directories(b2_gc INTERFACE
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)
