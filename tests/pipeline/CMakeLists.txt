# B-2 pipeline tests: the dirty-node closure algorithm + the partial
# deopt contract surfaces.
#
# Mirrors tests/{frontend,rbc,ir,passes,interp,baseline,codegen}/: one
# test executable (TestMain + unit suite) linking the pipeline library
# under test (b2::pipeline, which links b2::ir transitively). The tests
# construct minimal IR graphs by hand (same approach as tests/ir/) and
# exercise the closure algorithm's contract:
#
#   - seed dedup + dead-seed drop,
#   - forward closure through Data/Mem/FrameState/Parent roles,
#   - no propagation through Ctrl/None roles,
#   - side-effecting users always propagate dirty,
#   - budget cap enforcement,
#   - determinism (Rule 124).
add_executable(b2_pipeline_tests
  TestMain.cpp
  DirtyClosureTests.cpp
  RegionSafetyTests.cpp
)

target_link_libraries(b2_pipeline_tests PRIVATE b2::pipeline b2::ir)
target_compile_features(b2_pipeline_tests PRIVATE cxx_std_26)
set_target_properties(b2_pipeline_tests PROPERTIES CXX_EXTENSIONS OFF)

add_test(NAME pipeline_tests COMMAND b2_pipeline_tests)
set_tests_properties(pipeline_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
