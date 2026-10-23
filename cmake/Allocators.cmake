# The baseline allocators built by scripts/build-allocators.sh, read from its
# manifest at configure time, for tests that run once per allocator under
# LD_PRELOAD. Without the manifest (CI, other machines) there are none, and
# those tests are simply not added.
set(TIERALLOC_DEPS_MANIFEST ${PROJECT_SOURCE_DIR}/build-deps/manifest.json
    CACHE FILEPATH "Manifest written by scripts/build-allocators.sh")

# tieralloc_preloaded_allocators(<var>)
#   Sets <var> to the names of the allocators swapped in with LD_PRELOAD (glibc,
#   the system allocator, has no library and is left out). For each name, also
#   sets <var>_<name>_LIBRARY to its library and <var>_<name>_ENV to its runEnv
#   as NAME=value items, so tests run as the matrix does.
function(tieralloc_preloaded_allocators var)
  set(names "")
  if(EXISTS ${TIERALLOC_DEPS_MANIFEST})
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
        message(WARNING "${allocator} library missing (${library}); rerun build-allocators.sh")
        continue()
      endif()
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
      list(APPEND names ${allocator})
      set(${var}_${allocator}_LIBRARY ${library} PARENT_SCOPE)
      set(${var}_${allocator}_ENV "${runEnv}" PARENT_SCOPE)
    endforeach()
  endif()
  set(${var} ${names} PARENT_SCOPE)
endfunction()
