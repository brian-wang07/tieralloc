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
