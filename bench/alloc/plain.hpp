#pragma once

// Seam backend for the existing allocator configs: glibc, jemalloc, minmalloc,
// and tcmalloc. Turns a seam request into it's respective operator new/delete
// call.

#include "role.hpp"
#include <cstddef>
#include <new>
#include <nlohmann/json.hpp>
#include <string_view>

namespace tabench::alloc {

struct PlainBackend {
  static constexpr std::string_view name = "plain";

  template <Role R>
  [[nodiscard, gnu::always_inline]] static void *allocate(std::size_t bytes,
                                                          std::size_t align) {
    if (!overAligned(align)) {
      return ::operator new(bytes);
    } else {
      return ::operator new(bytes, std::align_val_t{align});
    }
  }

  template <Role R>
  [[gnu::always_inline]] static void deallocate(void *p, std::size_t bytes,
                                                std::size_t align) noexcept {
    // different allocators use different policies for sized deletes; specifying
    // the size explicitly bypasses this to benchmark raw free performance.
    if (!overAligned(align)) {
      ::operator delete(p, bytes);
    } else {
      ::operator delete(p, bytes, std::align_val_t{align});
    }
  }

  static void endPhase() noexcept {}

  // The allocator's own name and version are already in the record's
  // "allocator" field, from detectAllocator().
  static nlohmann::ordered_json extraStats() {
    nlohmann::ordered_json j;
    j["defaultNewAlignment"] = std::size_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__};
    j["sizedDelete"] = true;
    return j;
  }

private:
  static constexpr bool overAligned(std::size_t align) noexcept {
    return align > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
  }
};

} // namespace tabench::alloc
