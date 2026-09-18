# B-2 AOT: v0 stub directory.
#
# WHY THIS IS A STUB:
# docs/teams/ownership.yaml lists compiler/aot/ and tests/aot/ as the
# aot team's write paths. docs/aot_contract.md is the v0 contract; no
# code exists today. See docs/STATUS.md.
#
# The hard dependencies are: the T2 execution driver (Amendment B.5:
# "AOT uses the T2 pipeline - not a separate optimizer"), the deopt
# backend, and closure analysis. None of these exist today; this stub
# is the forward hook.
#
# This CMakeLists.txt declares an INTERFACE library so consumers can
# link b2::aot (today a no-op) and the v0 -> v1 transition can add real
# source files here without touching any consumer's build.

add_library(b2_aot INTERFACE)
add_library(b2::aot ALIAS b2_aot)
target_include_directories(b2_aot INTERFACE
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)
