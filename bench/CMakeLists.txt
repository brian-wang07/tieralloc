find_package(Threads REQUIRED)

# Harness: timing, perf counters, RSS, result output. Header-only to start;
# switch to STATIC and list sources if it grows .cpp files.
add_library(tieralloc_harness INTERFACE)
target_include_directories(tieralloc_harness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tieralloc_harness INTERFACE tieralloc_options Threads::Threads nlohmann_json)

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
# header. glibc/jemalloc/mimalloc are all "plain" and swapped via LD_PRELOAD.
set(TIERALLOC_BENCH_BACKENDS plain pools)

if(TIERALLOC_TCMALLOC_ROOT)
  find_path(TCMALLOC_INCLUDE_DIR tcmalloc/malloc_extension.h
            HINTS ${TIERALLOC_TCMALLOC_ROOT} PATH_SUFFIXES include NO_DEFAULT_PATH)
  find_library(TCMALLOC_LIBRARY NAMES tcmalloc
               HINTS ${TIERALLOC_TCMALLOC_ROOT} PATH_SUFFIXES lib lib64 NO_DEFAULT_PATH)
  if(TCMALLOC_INCLUDE_DIR AND TCMALLOC_LIBRARY)
    add_library(tcmalloc::tcmalloc UNKNOWN IMPORTED)
    set_target_properties(tcmalloc::tcmalloc PROPERTIES
      IMPORTED_LOCATION ${TCMALLOC_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${TCMALLOC_INCLUDE_DIR})
    list(APPEND TIERALLOC_BENCH_BACKENDS tcmalloc)
  else()
    message(WARNING "TIERALLOC_TCMALLOC_ROOT set but tcmalloc headers/library not found there")
  endif()
endif()
message(STATUS "bench backends: ${TIERALLOC_BENCH_BACKENDS}")

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
    target_compile_definitions(${target} PRIVATE TA_BACKEND_${BACKEND}=1)
    target_link_libraries(${target} PRIVATE tieralloc_harness)
    if(backend STREQUAL "tcmalloc")
      target_link_libraries(${target} PRIVATE tcmalloc::tcmalloc)
    endif()
  endforeach()
endfunction()

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
