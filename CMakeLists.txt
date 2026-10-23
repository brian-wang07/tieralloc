cmake_minimum_required(VERSION 3.25)
project(tieralloc VERSION 0.1.0 LANGUAGES CXX)

# The bench, profiler and tooling use C++26 (for contracts; GCC 16 is the
# project's compiler). tieralloc.hpp itself must stay C++20-compatible: its
# test targets (M2) override CXX_STANDARD to 20.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Benchmarks want optimized code with symbols (for perf and symbolization).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(TIERALLOC_BUILD_BENCH "Build tieralloc-bench workloads and microbenchmarks" ON)
option(TIERALLOC_BUILD_TRACE "Build the tieralloc-trace profiler runtime and its tests (Linux)" ON)
option(TIERALLOC_NATIVE "Compile with -march=native (recorded in results; off for portability)" OFF)
# For tests only, built with Clang (e.g. -DCMAKE_CXX_COMPILER=clang++
# -DTIERALLOC_SANITIZE=thread). Sanitized builds are never measured.
set(TIERALLOC_SANITIZE "" CACHE STRING "Sanitizer for every target: thread, address, undefined, or empty")
set_property(CACHE TIERALLOC_SANITIZE PROPERTY STRINGS "" thread address undefined)
if(TIERALLOC_SANITIZE AND NOT TIERALLOC_SANITIZE MATCHES "^(thread|address|undefined)$")
  message(FATAL_ERROR "TIERALLOC_SANITIZE must be thread, address, undefined or empty")
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "Non-Linux host: workloads build, but perf counters, LD_PRELOAD baselines "
                 "and /proc-based RSS are Linux only. Measured results must come from Linux.")
endif()

# Flags shared by every target in the project. -Werror=switch makes every
# switch over an enum (above all, a backend's policy for each Role) fail to
# build until it handles a newly added enumerator.
add_library(tieralloc_options INTERFACE)
target_compile_options(tieralloc_options INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror=switch -fno-omit-frame-pointer>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<BOOL:${TIERALLOC_NATIVE}>:-march=native>)
if(TIERALLOC_SANITIZE)
  target_compile_options(tieralloc_options INTERFACE -fsanitize=${TIERALLOC_SANITIZE})
  target_link_options(tieralloc_options INTERFACE -fsanitize=${TIERALLOC_SANITIZE})
endif()
# Baked into binaries so every result file can record how it was built.
target_compile_definitions(tieralloc_options INTERFACE
  TA_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  TA_BUILD_TYPE="$<CONFIG>"
  TA_NATIVE=$<BOOL:${TIERALLOC_NATIVE}>)

# Headers shared by the bench and the profiler (contract macros, counter reads).
# Include as "ta/<name>.hpp". Nothing in it may allocate: the profiler's preload
# shim uses it.
add_library(tieralloc_common INTERFACE)
target_include_directories(tieralloc_common INTERFACE ${PROJECT_SOURCE_DIR}/common)
target_link_libraries(tieralloc_common INTERFACE tieralloc_options)

# The library itself (include/tieralloc.hpp) arrives in M2.

include(cmake/Contracts.cmake)
include(cmake/Allocators.cmake)

enable_testing()

if(TIERALLOC_BUILD_BENCH)
  add_subdirectory(bench)
endif()

# The profiler needs /proc, perf_event_open and LD_PRELOAD, so it is Linux only.
if(TIERALLOC_BUILD_TRACE)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(trace)
  else()
    message(STATUS "tieralloc-trace is Linux only; skipping it on ${CMAKE_SYSTEM_NAME}")
  endif()
endif()
