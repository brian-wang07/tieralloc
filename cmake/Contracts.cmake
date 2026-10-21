# Contract checking (see common/ta/contracts.hpp). One setting drives three things
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
  message(STATUS "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} has no C++26 "
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
