# tieralloc-trace: the profiler runtime. The header-only core (timestamps, the
# record format and the ring) is shared by the preload shim and the tests.
add_library(tieralloc_trace_core INTERFACE)
target_include_directories(tieralloc_trace_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tieralloc_trace_core INTERFACE tieralloc_common)

# The LD_PRELOAD shim, libtieralloc-trace.so. Sanitizers intercept malloc
# themselves and would fight it, so sanitized builds skip it. Each flag closes
# a way for the shim to call back into its own hooks:
# - hidden visibility: only the hooks (TA_EXPORT) are exported;
# - -fno-builtin: GCC can't turn malloc + memset into a call to calloc;
# - -z now: no lazy binding (ld.so's resolver) in the middle of a hook;
# - quick_enforce: a violation traps, with no handler that could allocate.
if(NOT TIERALLOC_SANITIZE)
  add_library(tieralloc_trace SHARED src/shim.cpp)
  set_target_properties(tieralloc_trace PROPERTIES
    OUTPUT_NAME tieralloc-trace
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  target_compile_options(tieralloc_trace PRIVATE -fno-builtin)
  target_link_options(tieralloc_trace PRIVATE -Wl,-z,now)
  target_link_libraries(tieralloc_trace PRIVATE tieralloc_trace_core ${CMAKE_DL_LIBS})
  tieralloc_contract_semantic(tieralloc_trace quick_enforce)

  # Runs with the shim in LD_PRELOAD; registered with ctest once per
  # allocator. Building it builds the shim.
  find_package(Threads REQUIRED)
  add_executable(trace-shim-test test/shim_test.cpp)
  target_link_libraries(trace-shim-test PRIVATE tieralloc_trace_core Threads::Threads ${CMAKE_DL_LIBS})
  tieralloc_contract_semantic(trace-shim-test enforce)
  add_dependencies(trace-shim-test tieralloc_trace)

  # Once on glibc, then once per allocator from build-allocators.sh with the
  # shim listed first, as tracing runs will be.
  add_test(NAME trace-shim-test
           COMMAND ${CMAKE_COMMAND} -E env LD_PRELOAD=$<TARGET_FILE:tieralloc_trace>
                   $<TARGET_FILE:trace-shim-test>)
  # Link-level properties a runtime test can't see: exports, PLT callees,
  # -z now, initial-exec TLS. Uses the toolchain's binutils.
  add_test(NAME trace-shim-symbols
           COMMAND ${CMAKE_COMMAND} -E env NM=${CMAKE_NM} OBJDUMP=${CMAKE_OBJDUMP}
                   READELF=${CMAKE_READELF}
                   bash ${CMAKE_CURRENT_SOURCE_DIR}/test/check_shim_symbols.sh
                   $<TARGET_FILE:tieralloc_trace>)

  tieralloc_preloaded_allocators(preloaded)
  foreach(allocator IN LISTS preloaded)
    add_test(NAME trace-shim-test-${allocator}
             COMMAND ${CMAKE_COMMAND} -E env
                     "LD_PRELOAD=$<TARGET_FILE:tieralloc_trace> ${preloaded_${allocator}_LIBRARY}"
                     ${preloaded_${allocator}_ENV}
                     $<TARGET_FILE:trace-shim-test>)
  endforeach()

  # tieralloc-trace, the launcher and watcher. Its default --shim is this
  # build's, so building it builds the shim.
  add_executable(tieralloc_trace_cli src/launcher/main.cpp)
  set_target_properties(tieralloc_trace_cli PROPERTIES OUTPUT_NAME tieralloc-trace)
  target_include_directories(tieralloc_trace_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(tieralloc_trace_cli PRIVATE tieralloc_trace_core)
  target_compile_definitions(tieralloc_trace_cli PRIVATE
    TA_SHIM_PATH="$<TARGET_FILE:tieralloc_trace>")
  tieralloc_contract_semantic(tieralloc_trace_cli enforce)
  add_dependencies(tieralloc_trace_cli tieralloc_trace)

  # The launcher's command line; resolves the real shim, so it lives here.
  add_executable(trace-options-test test/options_test.cpp)
  target_include_directories(trace-options-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(trace-options-test PRIVATE tieralloc_trace_core)
  target_compile_definitions(trace-options-test PRIVATE
    TA_SHIM_PATH="$<TARGET_FILE:tieralloc_trace>")
  tieralloc_contract_semantic(trace-options-test enforce)
  add_dependencies(trace-options-test tieralloc_trace)
  add_test(NAME trace-options-test COMMAND trace-options-test)
endif()

add_executable(trace-tsc-test test/tsc_test.cpp)
target_link_libraries(trace-tsc-test PRIVATE tieralloc_trace_core)
tieralloc_contract_semantic(trace-tsc-test enforce)
add_test(NAME trace-tsc-test COMMAND trace-tsc-test)

add_executable(trace-format-test test/format_test.cpp)
target_link_libraries(trace-format-test PRIVATE tieralloc_trace_core)
tieralloc_contract_semantic(trace-format-test enforce)
add_test(NAME trace-format-test COMMAND trace-format-test)

add_executable(trace-ring-test test/ring_test.cpp)
target_link_libraries(trace-ring-test PRIVATE tieralloc_trace_core)
tieralloc_contract_semantic(trace-ring-test enforce)
add_test(NAME trace-ring-test COMMAND trace-ring-test)

add_executable(trace-pool-test test/pool_test.cpp)
target_link_libraries(trace-pool-test PRIVATE tieralloc_trace_core)
tieralloc_contract_semantic(trace-pool-test enforce)
add_test(NAME trace-pool-test COMMAND trace-pool-test)
