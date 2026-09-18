cmake_minimum_required(VERSION 3.24)

project(B2
  VERSION 0.1.0
  DESCRIPTION "B-2 - AI-built Java runtime and tiered compiler"
  LANGUAGES CXX)

# The whole project is C++26.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)

option(B2_BUILD_TOOLS "Build command-line tools (b2parse)" ON)
option(B2_BUILD_TESTS "Build the test suite" ON)
option(B2_BUILD_FUZZERS "Build libFuzzer harnesses (opt-in; requires clang + -fsanitize=fuzzer)" OFF)

enable_testing()

add_subdirectory(compiler/frontend)
add_subdirectory(compiler/rbc)
add_subdirectory(compiler/ir)
add_subdirectory(compiler/passes)
add_subdirectory(compiler/interp)
add_subdirectory(compiler/baseline)
add_subdirectory(compiler/codegen)

# v0 stub directories — see each subdir's README.md. These exist so
# docs/teams/ownership.yaml is consistent with the tree; the INTERFACE
# libraries declared here are no-ops today (no objects, no headers).
# The v0 -> v1 transition for any of these goes through the message
# system per docs/teams/messaging.md and requires an ADVISORY to every
# affected team before real source files land here.
add_subdirectory(compiler/gc)
add_subdirectory(compiler/regalloc)
add_subdirectory(compiler/aot)
add_subdirectory(compiler/pipeline)

if(B2_BUILD_TESTS)
  add_subdirectory(tests/frontend)
  add_subdirectory(tests/rbc)
  add_subdirectory(tests/ir)
  add_subdirectory(tests/passes)
  add_subdirectory(tests/interp)
  add_subdirectory(tests/baseline)
  add_subdirectory(tests/codegen)
  add_subdirectory(tests/pipeline)
  # v0 stub test directories — no add_test() call today, but the
  # directory exists so the ownership map is consistent.
  add_subdirectory(tests/gc)
  add_subdirectory(tests/regalloc)
  add_subdirectory(tests/aot)
endif()

# Opt-in fuzzer harnesses. See fuzz/README.md. Off by default because
# the default toolchain (g++-14) does not have libFuzzer; turning this
# ON without a suitable compiler is a hard configure error, not a
# silent skip.
if(B2_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
