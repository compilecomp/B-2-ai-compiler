# B-2 Pipeline: v0.1 (passes team).
#
# WHY THIS IS NOT A STUB ANYMORE:
# docs/teams/ownership.yaml lists compiler/pipeline/ as a passes-team
# write path. The v0 contract surfaces (include/b2/pipeline/*.h) landed
# in MSG-20260918-007 (DependencyIndex, Region, PartialDeopt); the v0.1
# implementation of the dirty-node closure algorithm (Section 5 of
# docs/partial_deopt_contract.md) lands here in MSG-20260918-008. The
# remaining v0 -> v1 transition items (the region builder, the region
# safety checks, the partial rebuild, the atomic activation, the
# guard-failure integration) are open work tracked in docs/STATUS.md
# item 13.
#
# The library is a STATIC (not INTERFACE) starting at v0.1 because the
# dirty closure has a real implementation; consumers (the engine's trap
# handler in the v0 -> v1 transition) link b2::pipeline to call
# expandDirtyClosure().

add_library(b2_pipeline STATIC
  src/DependencyIndex.cpp
  src/DirtyClosure.cpp
  src/RegionSafety.cpp
)
add_library(b2::pipeline ALIAS b2_pipeline)
target_include_directories(b2_pipeline PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)
# The v0 contract surfaces (DependencyIndex.h, Region.h, PartialDeopt.h,
# DirtyClosure.h) depend on the IR's public types (ir::Graph, ir::Node,
# ir::DependencyId, ir::FrameStateDesc, etc.). Link b2::ir so consumers
# get the IR's include path transitively.
target_link_libraries(b2_pipeline PUBLIC b2::ir)
target_compile_features(b2_pipeline PUBLIC cxx_std_26)
set_target_properties(b2_pipeline PROPERTIES CXX_EXTENSIONS OFF)

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
