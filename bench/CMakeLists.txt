find_package(Threads REQUIRED)

# Harness: timing, perf counters, RSS, result output. Header-only to start;
# switch to STATIC and list sources if it grows .cpp files.
add_library(tieralloc_harness INTERFACE)
target_include_directories(tieralloc_harness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# dlsym (allocator detection) needs libdl on glibc older than 2.34.
target_link_libraries(tieralloc_harness INTERFACE tieralloc_common Threads::Threads nlohmann_json
                      ${CMAKE_DL_LIBS})

# nlohmann/json, vendored single header (see third_party/VERSIONS). SYSTEM keeps
# our warning flags from firing inside it. Include as <nlohmann/json.hpp>.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/third_party)

# Run first on any new machine (and via ctest): clock, region, batch and pinning checks.
add_executable(harness-selftest harness_selftest.cpp)
target_link_libraries(harness-selftest PRIVATE tieralloc_harness)
tieralloc_contract_semantic(harness-selftest enforce)
add_test(NAME harness-selftest COMMAND harness-selftest)

# Allocation backends. Each workload is compiled once per backend from the same
# source; the backend is selected by a TA_BACKEND_<NAME> define seen by the seam
# header. glibc, jemalloc, mimalloc and tcmalloc are all "plain" and swapped via
# LD_PRELOAD (see scripts/build-allocators.sh). Hinted tcmalloc, which needs
# its hot/cold operator new, is deferred until tieralloc's own hints exist.
set(TIERALLOC_BENCH_BACKENDS plain pools)

message(STATUS "bench backends: ${TIERALLOC_BENCH_BACKENDS}")

# tieralloc_add_workload(<name> <source> [SMOKE <args>...])
#   -> bin/<name>-<backend> for each backend, built with the timing semantic,
#   and bin/<name>-<backend>-checked with contracts and seam checks enforced.
#   ctest runs only the checked builds, with SMOKE's small parameters, so a
#   workload that frees with the wrong role or size fails a test instead of
#   passing silently in a timing build.
# Sources not written yet are skipped so the tree configures while M0 is in progress.
function(tieralloc_add_workload name source)
  cmake_parse_arguments(PARSE_ARGV 2 workload "" "" "SMOKE")
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    message(STATUS "bench: skipping ${name} (${source} not written yet)")
    return()
  endif()
  foreach(backend IN LISTS TIERALLOC_BENCH_BACKENDS)
    string(TOUPPER ${backend} BACKEND)
    foreach(kind timing checked)
      if(kind STREQUAL "timing")
        set(target ${name}-${backend})
        set(semantic ${TIERALLOC_CONTRACT_SEMANTIC})
      else()
        set(target ${name}-${backend}-checked)
        set(semantic enforce)
      endif()
      add_executable(${target} ${source})
      target_compile_definitions(${target} PRIVATE TA_BACKEND_${BACKEND}=1)
      target_link_libraries(${target} PRIVATE tieralloc_harness)
      tieralloc_contract_semantic(${target} ${semantic})
    endforeach()
    add_test(NAME ${name}-${backend}
             COMMAND ${name}-${backend}-checked ${workload_SMOKE}
                     --counters=off --warmup-ms=10 --out=/dev/null)
  endforeach()
endfunction()

# Seam tests: one binary per backend, contracts and checks always enforced.
set(TIERALLOC_SEAM_TEST_BACKENDS plain pools)
set(TIERALLOC_SEAM_TEST_SOURCE alloc_seam_test.cpp)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${TIERALLOC_SEAM_TEST_SOURCE})
  foreach(backend IN LISTS TIERALLOC_SEAM_TEST_BACKENDS)
    set(target seam-test-${backend})
    add_executable(${target} ${TIERALLOC_SEAM_TEST_SOURCE})
    string(TOUPPER ${backend} BACKEND)
    target_compile_definitions(${target} PRIVATE TA_BACKEND_${BACKEND}=1)
    target_link_libraries(${target} PRIVATE tieralloc_harness)
    tieralloc_contract_semantic(${target} enforce)
    add_test(NAME ${target} COMMAND ${target})
  endforeach()
else()
  message(STATUS "bench: skipping seam tests (${TIERALLOC_SEAM_TEST_SOURCE} not written yet)")
endif()

# The plain seam test again under each preloaded allocator built by
# scripts/build-allocators.sh (see cmake/Allocators.cmake).
if(TARGET seam-test-plain)
  tieralloc_preloaded_allocators(preloaded)
  foreach(allocator IN LISTS preloaded)
    add_test(NAME seam-test-plain-${allocator}
             COMMAND ${CMAKE_COMMAND} -E env LD_PRELOAD=${preloaded_${allocator}_LIBRARY}
                     ${preloaded_${allocator}_ENV}
                     $<TARGET_FILE:seam-test-plain> --expect-allocator=${allocator})
  endforeach()
  if(preloaded)
    message(STATUS "bench: seam tests under preload: ${preloaded}")
  endif()
endif()

tieralloc_add_workload(churn workloads/churn.cpp
  SMOKE --phases=10 --phase-bytes=262144 --band-phases=1 --survivor-phases=3)
tieralloc_add_workload(orderbook workloads/orderbook.cpp
  SMOKE --orders=2000 --messages=20000 --warmup-messages=2000
        --phase-messages=1000 --sample-messages=5000)
tieralloc_add_workload(chase workloads/chase.cpp
  SMOKE --nodes=2000 --laps=2)

# Raw malloc/free cost. Only the plain backend: allocators are swapped via LD_PRELOAD.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/micro/malloc_cost.cpp)
  add_executable(malloc-cost micro/malloc_cost.cpp)
  target_link_libraries(malloc-cost PRIVATE tieralloc_harness)
  tieralloc_contract_semantic(malloc-cost ${TIERALLOC_CONTRACT_SEMANTIC})
  # Smoke tests: each pattern runs and writes a record. Never timing checks.
  foreach(pattern pair burst-lifo burst-fifo churn)
    add_test(NAME malloc-cost-${pattern}
             COMMAND malloc-cost --pattern=${pattern} --batches=3
                     --ops-per-batch=256 --warmup=1 --counters=off
                     --warmup-ms=10)
  endforeach()
else()
  message(STATUS "bench: skipping malloc-cost (micro/malloc_cost.cpp not written yet)")
endif()

# The campaign scripts (scripts/bench_*.py): unit tests on synthetic records,
# then a smoke campaign through the real binaries (bench/matrix-smoke.json),
# the summary and, with matplotlib, the plots.
find_package(Python3 3.11 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME bench-scripts
           COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/scripts/test_bench.py
                   --build ${CMAKE_BINARY_DIR})
else()
  message(STATUS "bench: no Python 3.11+, skipping the bench-scripts test")
endif()
