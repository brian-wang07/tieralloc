# tieralloc-trace: the profiler runtime. The header-only core (timestamps, and
# later the record format and the ring) is shared by the preload shim and the
# tests; the shim itself is a SHARED library added in step 2.
add_library(tieralloc_trace_core INTERFACE)
target_include_directories(tieralloc_trace_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tieralloc_trace_core INTERFACE tieralloc_common)

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
