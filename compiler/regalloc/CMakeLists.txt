# B-2 RegAlloc: v0 stub directory.
#
# WHY THIS IS A STUB:
# docs/teams/ownership.yaml lists compiler/regalloc/ and tests/regalloc/
# as the regalloc team's write paths. docs/regalloc_contract.md is the
# v0 contract; no code exists today. See docs/STATUS.md.
#
# The hard dependencies are: the T2 execution driver (lowers an IR graph
# to machine code, then to the MIR the allocator consumes) and the MIR
# contract between codegen and regalloc. Neither exists today; this stub
# is the forward hook.
#
# This CMakeLists.txt declares an INTERFACE library so consumers can
# link b2::regalloc (today a no-op) and the v0 -> v1 transition can add
# real source files here without touching any consumer's build.

add_library(b2_regalloc INTERFACE)
add_library(b2::regalloc ALIAS b2_regalloc)
target_include_directories(b2_regalloc INTERFACE
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)
