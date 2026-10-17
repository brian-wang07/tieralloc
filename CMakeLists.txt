cmake_minimum_required(VERSION 3.25)
project(tieralloc VERSION 0.1.0 LANGUAGES CXX)

# The bench, profiler and tooling use C++23. tieralloc.hpp itself must stay
# C++20-compatible: its test targets (M2) override CXX_STANDARD to 20.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Benchmarks want optimized code with symbols (for perf and symbolization).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(TIERALLOC_BUILD_BENCH "Build tieralloc-bench workloads and microbenchmarks" ON)
option(TIERALLOC_NATIVE "Compile with -march=native (recorded in results; off for portability)" OFF)
set(TIERALLOC_TCMALLOC_ROOT "" CACHE PATH
    "Prefix of a Google tcmalloc build (headers + libtcmalloc); enables the hinted-tcmalloc variant")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "Non-Linux host: workloads build, but perf counters, LD_PRELOAD baselines "
                 "and /proc-based RSS are Linux only. Measured results must come from Linux.")
endif()

# Flags shared by every target in the project.
add_library(tieralloc_options INTERFACE)
target_compile_options(tieralloc_options INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -fno-omit-frame-pointer>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<BOOL:${TIERALLOC_NATIVE}>:-march=native>)
# Baked into binaries so every result file can record how it was built.
target_compile_definitions(tieralloc_options INTERFACE
  TA_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  TA_BUILD_TYPE="$<CONFIG>"
  TA_NATIVE=$<BOOL:${TIERALLOC_NATIVE}>)

# The library itself (include/tieralloc.hpp) arrives in M2.

enable_testing()

if(TIERALLOC_BUILD_BENCH)
  add_subdirectory(bench)
endif()
