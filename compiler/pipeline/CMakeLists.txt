# B-2 Pipeline: v0 contract surface (passes team).
#
# WHY THIS IS A STUB:
# docs/teams/ownership.yaml lists compiler/pipeline/ as a passes-team
# write path (the comment there reads "Special pass designs ..."). The
# actual pass bodies live in compiler/passes/ today; this directory is
# the forward home for the T2 execution driver (landed in
# MSG-20260918-005 as b2t2, the consumer end of the pipeline) and the
# partial deopt layer (the v0 contract landed in MSG-20260918-007; see
# docs/partial_deopt_contract.md).
#
# The v0 ships contract surfaces only (headers, no .cpp files). The
# headers under include/b2/pipeline/ (DependencyIndex.h, Region.h,
# PartialDeopt.h) declare the type definitions and the API the runtime
# will call; the implementation is the v0 -> v1 transition's work.
# The v0 default `enable_partial_deopt = false` short-circuits every
# call to the partial deopt layer; the engine's trap handler goes
# straight to the existing T0 deopt path (the v0 method-granularity
# invalidation, docs/deopt_backend.md Section 17).
#
# This CMakeLists.txt declares an INTERFACE library so:
#   - the directory is consistent with the ownership map,
#   - consumers can link b2::pipeline (today a no-op that exposes
#     the include path; the v0 -> v1 transition adds real source
#     files here without touching any consumer's build),
#   - the v0 -> v1 transition for any subsystem here goes through
#     the message system per docs/teams/messaging.md and requires
#     an ADVISORY to every affected team before real source files
#     land here.
#
# DO NOT add source files here without landing
# docs/partial_deopt_contract.md's v1 implementation + an INFO message
# to the integrator-owned message space.

add_library(b2_pipeline INTERFACE)
add_library(b2::pipeline ALIAS b2_pipeline)
target_include_directories(b2_pipeline INTERFACE
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)
# The v0 contract surfaces (DependencyIndex.h, Region.h, PartialDeopt.h)
# depend on the IR's public types (ir::Graph, ir::Node, ir::DependencyId,
# ir::FrameStateDesc, etc.). Link b2::ir so consumers get the IR's
# include path transitively.
target_link_libraries(b2_pipeline INTERFACE b2::ir)

if(B2_BUILD_TOOLS)
  # Compile check: the v0 contract surfaces are headers-only with no
  # consumers yet (the engine's trap handler will be the first in the
  # v0 -> v1 transition). This TU is the mechanical check that the
  # headers parse cleanly under the repo's strict flags and that the
  # type definitions and APIs are usable. The v0 -> v1 transition
  # replaces this with real unit tests under tests/pipeline/.
  add_executable(pipeline_compile_check tools/pipeline_compile_check.cpp)
  target_link_libraries(pipeline_compile_check PRIVATE b2::pipeline b2::codegen)
  target_compile_features(pipeline_compile_check PRIVATE cxx_std_26)
  set_target_properties(pipeline_compile_check PROPERTIES CXX_EXTENSIONS OFF)
  add_test(NAME pipeline_compile_check COMMAND pipeline_compile_check)
  set_tests_properties(pipeline_compile_check PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
