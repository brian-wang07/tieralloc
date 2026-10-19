#pragma once

// Seam backend for the existing allocator configs: glibc, jemalloc, minmalloc,
// and tcmalloc. Turns a seam request into it's respective operator new/delete
// call.

#include "backend.hpp"
#include "role.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <new>
#include <string_view>

namespace tabench::alloc {

struct PlainBackend {
  static constexpr std::string_view name = "plain";

  template <Role R>
  [[nodiscard, gnu::always_inline]] static void *allocate(std::size_t bytes,
                                                          std::size_t align) {
    if (!overAligned(align))
      return ::operator new(requestBytes(bytes, align));
    return ::operator new(bytes, std::align_val_t{align});
  }

  template <Role R>
  [[gnu::always_inline]] static void deallocate(void *p, std::size_t bytes,
                                                std::size_t align) noexcept {
    if (!overAligned(align))
      ::operator delete(p, requestBytes(bytes, align));
    else
      ::operator delete(p, bytes, std::align_val_t{align});
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
  // operator new(n) only guarantees alignment for an object of size n. That
  // covers any align up to the default when n is a multiple of it, which the
  // seam requires, except n == 0: a 0-byte object needs no alignment, and
  // jemalloc, mimalloc and tcmalloc do return 8-aligned blocks for it.
  static constexpr bool overAligned(std::size_t align) noexcept {
    return align > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
  }

  // The size passed to plain operator new and its sized delete. A 0-byte
  // request asks for align bytes, the smallest size whose alignment new must
  // honour; both directions compute it the same way, so the sizes match.
  static constexpr std::size_t requestBytes(std::size_t bytes,
                                            std::size_t align) noexcept {
    return bytes == 0 ? align : bytes;
  }
};
static_assert(SeamBackend<PlainBackend>);

} // namespace tabench::alloc
