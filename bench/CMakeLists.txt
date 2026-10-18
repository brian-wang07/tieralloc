find_package(Threads REQUIRED)

# Harness: timing, perf counters, RSS, result output. Header-only to start;
# switch to STATIC and list sources if it grows .cpp files.
add_library(tieralloc_harness INTERFACE)
target_include_directories(tieralloc_harness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# dlsym (allocator detection) needs libdl on glibc older than 2.34.
target_link_libraries(tieralloc_harness INTERFACE tieralloc_options Threads::Threads nlohmann_json
                      ${CMAKE_DL_LIBS})

# nlohmann/json, vendored single header (see third_party/VERSIONS). SYSTEM keeps
# our warning flags from firing inside it. Include as <nlohmann/json.hpp>.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/third_party)

# Run first on any new machine (and via ctest): clock, region, batch and pinning checks.
add_executable(harness-selftest harness_selftest.cpp)
target_link_libraries(harness-selftest PRIVATE tieralloc_harness)
add_test(NAME harness-selftest COMMAND harness-selftest)

# Allocation backends. Each workload is compiled once per backend from the same
# source; the backend is selected by a TA_BACKEND_<NAME> define seen by the seam
# header. glibc, jemalloc, mimalloc and tcmalloc are all "plain" and swapped via
# LD_PRELOAD (see scripts/build-allocators.sh). Hinted tcmalloc, which needs
# its hot/cold operator new, is deferred until tieralloc's own hints exist.
set(TIERALLOC_BENCH_BACKENDS plain pools)

message(STATUS "bench backends: ${TIERALLOC_BENCH_BACKENDS}")

# Seam debug checks (owner thread, free underflow, pool ownership) in workloads.
# Set per target rather than from NDEBUG so a Debug build can still be timed
# and the result says which it was. Never on for timing runs. The seam tests
# always enable them.
option(TIERALLOC_SEAM_CHECKS "Enable seam debug checks in workloads (not for timing runs)" OFF)

# tieralloc_add_workload(<name> <source>) -> bin/<name>-<backend> for each backend.
# Sources not written yet are skipped so the tree configures while M0 is in progress.
function(tieralloc_add_workload name source)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    message(STATUS "bench: skipping ${name} (${source} not written yet)")
    return()
  endif()
  foreach(backend IN LISTS TIERALLOC_BENCH_BACKENDS)
    set(target ${name}-${backend})
    add_executable(${target} ${source})
    string(TOUPPER ${backend} BACKEND)
    target_compile_definitions(${target} PRIVATE TA_BACKEND_${BACKEND}=1
                               TA_SEAM_CHECKS=$<BOOL:${TIERALLOC_SEAM_CHECKS}>)
    target_link_libraries(${target} PRIVATE tieralloc_harness)
  endforeach()
endfunction()

# Seam tests: one binary per backend, checks always on.
set(TIERALLOC_SEAM_TEST_SOURCE alloc_seam_test.cpp)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${TIERALLOC_SEAM_TEST_SOURCE})
  foreach(backend IN LISTS TIERALLOC_BENCH_BACKENDS)
    set(target seam-test-${backend})
    add_executable(${target} ${TIERALLOC_SEAM_TEST_SOURCE})
    string(TOUPPER ${backend} BACKEND)
    target_compile_definitions(${target} PRIVATE TA_BACKEND_${BACKEND}=1 TA_SEAM_CHECKS=1)
    target_link_libraries(${target} PRIVATE tieralloc_harness)
    add_test(NAME ${target} COMMAND ${target})
  endforeach()
else()
  message(STATUS "bench: skipping seam tests (${TIERALLOC_SEAM_TEST_SOURCE} not written yet)")
endif()

# The plain seam test again under each preloaded allocator built by
# scripts/build-allocators.sh. Read from its manifest at configure time; without
# the manifest (CI, other machines) these tests are simply not added.
set(TIERALLOC_DEPS_MANIFEST ${PROJECT_SOURCE_DIR}/build-deps/manifest.json
    CACHE FILEPATH "Manifest written by scripts/build-allocators.sh")
if(TARGET seam-test-plain AND EXISTS ${TIERALLOC_DEPS_MANIFEST})
  # Reconfigure when the allocators are rebuilt.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TIERALLOC_DEPS_MANIFEST})
  file(READ ${TIERALLOC_DEPS_MANIFEST} manifest)
  string(JSON allocatorCount LENGTH "${manifest}" allocators)
  math(EXPR lastAllocator "${allocatorCount} - 1")
  foreach(i RANGE ${lastAllocator})
    string(JSON allocator MEMBER "${manifest}" allocators ${i})
    # glibc's library is null: it is the system allocator and needs no preload.
    string(JSON library ERROR_VARIABLE noLibrary GET "${manifest}" allocators ${allocator} library)
    if(noLibrary OR library STREQUAL "" OR library STREQUAL "null")
      continue()
    endif()
    if(NOT EXISTS ${library})
      message(WARNING "bench: ${allocator} library missing (${library}); rerun build-allocators.sh")
      continue()
    endif()
    add_test(NAME seam-test-plain-${allocator}
             COMMAND ${CMAKE_COMMAND} -E env LD_PRELOAD=${library}
                     $<TARGET_FILE:seam-test-plain> --expect-allocator=${allocator})
    list(APPEND preloaded ${allocator})
  endforeach()
  message(STATUS "bench: seam tests under preload: ${preloaded}")
endif()

tieralloc_add_workload(churn     workloads/churn.cpp)
tieralloc_add_workload(orderbook workloads/orderbook.cpp)
tieralloc_add_workload(chase     workloads/chase.cpp)

# Raw malloc/free cost. Only the plain backend: allocators are swapped via LD_PRELOAD.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/micro/malloc_cost.cpp)
  add_executable(malloc-cost micro/malloc_cost.cpp)
  target_link_libraries(malloc-cost PRIVATE tieralloc_harness)
else()
  message(STATUS "bench: skipping malloc-cost (micro/malloc_cost.cpp not written yet)")
endif()
