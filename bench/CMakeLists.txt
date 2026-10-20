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

# Contract checking (see harness/contracts.hpp). One setting drives three things
# per target: GCC's -fcontract-evaluation-semantic, TA_CONTRACT_SEMANTIC for the
# result record, and TA_SEAM_CHECKS for the manual checks that write state and
# so cannot be contracts (the seam's owner thread). Workloads take the cache
# value; timing runs must use ignore, and the runner rejects anything else.
# Tests always use enforce.
set(TIERALLOC_CONTRACT_SEMANTICS ignore observe enforce quick_enforce)
set(TIERALLOC_CONTRACT_SEMANTIC ignore CACHE STRING
    "Contract evaluation semantic for workloads: ${TIERALLOC_CONTRACT_SEMANTICS}")
set_property(CACHE TIERALLOC_CONTRACT_SEMANTIC PROPERTY STRINGS ${TIERALLOC_CONTRACT_SEMANTICS})
if(NOT TIERALLOC_CONTRACT_SEMANTIC IN_LIST TIERALLOC_CONTRACT_SEMANTICS)
  message(FATAL_ERROR "TIERALLOC_CONTRACT_SEMANTIC must be one of: ${TIERALLOC_CONTRACT_SEMANTICS}")
endif()

# C++26 contracts as these macros use them first appear in GCC 16. Other
# compilers (Clang for tooling and sanitizers) take the macros' fallbacks, so
# they get no flags and TA_CONTRACT_SEMANTIC reports "none".
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  set(TIERALLOC_HAS_CONTRACTS ON)
else()
  set(TIERALLOC_HAS_CONTRACTS OFF)
  message(STATUS "bench: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} has no C++26 "
                 "contracts; measured results must come from GCC 16")
endif()

# tieralloc_contract_semantic(<target> <semantic>)
function(tieralloc_contract_semantic target semantic)
  if(NOT semantic IN_LIST TIERALLOC_CONTRACT_SEMANTICS)
    message(FATAL_ERROR "${target}: unknown contract semantic '${semantic}'")
  endif()
  if(semantic STREQUAL "ignore")
    set(seamChecks 0)
  else()
    set(seamChecks 1)
  endif()
  target_compile_definitions(${target} PRIVATE TA_SEAM_CHECKS=${seamChecks})
  if(TIERALLOC_HAS_CONTRACTS)
    target_compile_options(${target} PRIVATE -fcontract-evaluation-semantic=${semantic})
    target_compile_definitions(${target} PRIVATE TA_CONTRACT_SEMANTIC="${semantic}")
    # GCC 16 ships the default violation handler in libstdc++exp. ignore emits
    # no handler calls, so timing builds don't link the experimental library.
    if(NOT semantic STREQUAL "ignore")
      target_link_libraries(${target} PRIVATE stdc++exp)
    endif()
  endif()
endfunction()

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
    # The allocator's runEnv (e.g. tcmalloc's), so tests run as the matrix does.
    set(runEnv "")
    string(JSON runEnvCount ERROR_VARIABLE noRunEnv LENGTH "${manifest}"
           allocators ${allocator} runEnv)
    if(NOT noRunEnv AND runEnvCount GREATER 0)
      math(EXPR lastRunEnv "${runEnvCount} - 1")
      foreach(j RANGE ${lastRunEnv})
        string(JSON name MEMBER "${manifest}" allocators ${allocator} runEnv ${j})
        string(JSON value GET "${manifest}" allocators ${allocator} runEnv ${name})
        list(APPEND runEnv "${name}=${value}")
      endforeach()
    endif()
    add_test(NAME seam-test-plain-${allocator}
             COMMAND ${CMAKE_COMMAND} -E env LD_PRELOAD=${library} ${runEnv}
                     $<TARGET_FILE:seam-test-plain> --expect-allocator=${allocator})
    list(APPEND preloaded ${allocator})
  endforeach()
  message(STATUS "bench: seam tests under preload: ${preloaded}")
endif()

tieralloc_add_workload(churn workloads/churn.cpp
  SMOKE --phases=10 --phase-bytes=262144 --band-phases=1 --survivor-phases=3)
tieralloc_add_workload(orderbook workloads/orderbook.cpp)
tieralloc_add_workload(chase     workloads/chase.cpp)

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
