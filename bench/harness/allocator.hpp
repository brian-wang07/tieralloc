#pragma once

// Which allocator actually serves malloc in this process. A wrong LD_PRELOAD
// path makes ld.so print a warning and carry on with glibc, so every record
// says what really ran and the runner checks it against the configuration.

#include <cstddef>
#include <string>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif
#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif

namespace tabench {

struct AllocatorInfo {
  std::string name; // jemalloc, mimalloc, tcmalloc, gperftools, glibc, system
  std::string version; // Empty if the allocator does not report one.
};

namespace detail {

inline void *findSymbol(const char *name) noexcept {
#if defined(__linux__) || defined(__APPLE__)
  return ::dlsym(RTLD_DEFAULT, name);
#else
  (void)name;
  return nullptr;
#endif
}

} // namespace detail

// Probes for a symbol each allocator exports and no other does. Call once at
// startup; it is not cheap and the answer cannot change.
inline AllocatorInfo detectAllocator() {
  using Mallctl =
      int (*)(const char *, void *, std::size_t *, void *, std::size_t);
  if (void *symbol = detail::findSymbol("mallctl")) {
    const char *version = nullptr;
    std::size_t length = sizeof(version);
    if (reinterpret_cast<Mallctl>(symbol)("version", &version, &length, nullptr,
                                          0) == 0 &&
        version)
      return {"jemalloc", version};
    return {"jemalloc", ""};
  }
  if (void *symbol = detail::findSymbol("mi_version")) {
    // MI_MALLOC_VERSION: major * 10000 + minor * 100 + patch, e.g. 30503.
    int version = reinterpret_cast<int (*)()>(symbol)();
    return {"mimalloc", std::to_string(version / 10000) + "." +
                            std::to_string(version / 100 % 100) + "." +
                            std::to_string(version % 100)};
  }
  if (detail::findSymbol("MallocExtension_Internal_GetNumericProperty"))
    return {"tcmalloc", ""};
  // gperftools' older tcmalloc is a different allocator; name it so it can
  // never pass for Google tcmalloc.
  if (auto version =
          reinterpret_cast<const char *(*)(int *, int *, const char **)>(
              detail::findSymbol("tc_version"))) {
    int major = 0, minor = 0;
    const char *patch = nullptr;
    return {"gperftools", version(&major, &minor, &patch)};
  }
#if defined(__GLIBC__)
  return {"glibc", gnu_get_libc_version()};
#else
  return {"system", ""};
#endif
}

} // namespace tabench
