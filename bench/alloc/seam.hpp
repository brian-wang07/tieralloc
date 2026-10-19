#pragma once

// The allocation seam: the only allocation header a workload includes. Each
// target is built against one backend, chosen by TA_BACKEND_* (plain: the
// global operator new of whichever allocator is preloaded; pools: hand-written
// pools), so the same workload source runs in every configuration.
//
// Every allocation carries a Role, taken from roleOf<T>, named at the call
// site, or chosen at run time with makeAs()/destroyAs(). A free must repeat
// the role, size and alignment of its allocation; destroy(), destroyAs() and
// Allocator do that for you. All routes go through allocate()/deallocate(),
// which hold the contracts and the stats, so backends need neither.
//
// Single-threaded, like the stats it updates.

#include "harness/contracts.hpp"
#include "role.hpp"
#include "stats.hpp"

#include <nlohmann/json.hpp>

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// The backend header must be included here, outside the namespace: it has its
// own includes, which would otherwise land inside tabench::alloc.
#if defined(TA_BACKEND_PLAIN) + defined(TA_BACKEND_POOLS) != 1
#error "define exactly one TA_BACKEND_* (CMake does this per target)"
#endif
#if defined(TA_BACKEND_PLAIN)
#include "plain.hpp"
#elif defined(TA_BACKEND_POOLS)
#include "pools.hpp"
#endif

namespace tabench::alloc {

template <class B>
concept SeamBackend = requires(void *p, std::size_t n) {
  { B::name } -> std::convertible_to<std::string_view>;
  { B::template allocate<Role::Default>(n, n) } -> std::same_as<void *>;
  { B::template deallocate<Role::Default>(p, n, n) } noexcept;
  { B::endPhase() } noexcept;
  { B::extraStats() } -> std::same_as<nlohmann::ordered_json>;
};

#if defined(TA_BACKEND_PLAIN)
using Backend = PlainBackend;
#elif defined(TA_BACKEND_POOLS)
using Backend = PoolsBackend;
#endif
static_assert(SeamBackend<Backend>);

// what make() can build
template <class T>
concept Makeable =
    std::is_object_v<T> && !std::is_array_v<T> && !std::is_abstract_v<T>;

// what destroy() can free. Freeing a derived object through a base pointer
// would pass the base's size, so polymorphic types must be final.
template <class T>
concept Destroyable = std::is_nothrow_destructible_v<T> &&
                      (!std::is_polymorphic_v<T> || std::is_final_v<T>);

template <Role R, SeamBackend B = Backend>
[[nodiscard]] void *allocate(const std::size_t bytes, const std::size_t align)
    TA_PRE(std::has_single_bit(align)) TA_PRE(bytes % align == 0)
    TA_POST(r,
            r != nullptr && reinterpret_cast<std::uintptr_t>(r) % align == 0) {
  // allocate and then record, to never count a bad_alloc
  void *p = B::template allocate<R>(bytes, align);
  seamStats.recordAllocate<R>(bytes);
  return p;
}

template <Role R, SeamBackend B = Backend>
void deallocate(void *p, std::size_t bytes, std::size_t align) noexcept
    TA_PRE(p != nullptr) TA_PRE(std::has_single_bit(align))
    TA_PRE(bytes % align == 0)
    TA_PRE(reinterpret_cast<std::uintptr_t>(p) % align == 0) {
  // record first, so that underflow precondition fires before a bad free
  // reaches the allocator.
  seamStats.recordFree<R>(bytes);
  B::template deallocate<R>(p, bytes, align);
}

template <Makeable T, Role R = roleOf<std::remove_cv_t<T>>, class... Args>
[[nodiscard]] T *make(Args &&...args) {
  // precondition always holds as sizeof(T) is a multiple of alignof(T).
  void *p = allocate<R>(sizeof(T), alignof(T));
  // if ctor throws, deallocate and rethrow
  if constexpr (!std::is_nothrow_constructible_v<T, Args...>) {
    try {
      return std::construct_at(static_cast<std::remove_cv_t<T> *>(p),
                               std::forward<Args>(args)...);
    } catch (...) {
      deallocate<R>(p, sizeof(T), alignof(T));
      throw;
    }
  } else
    return std::construct_at(static_cast<std::remove_cv_t<T> *>(p),
                             std::forward<Args>(args)...);
}

template <Destroyable T, Role R = roleOf<std::remove_cv_t<T>>>
void destroy(T *p) noexcept {
  if (p == nullptr)
    return;
  std::destroy_at(p);
  // const cast in the big 26 is crazy: see std::voidify implementation
  deallocate<R>(const_cast<void *>(static_cast<const volatile void *>(p)),
                sizeof(T), alignof(T));
}

// The role passed to destroyAs must be the same as that passed to makeAs.
template <Makeable T, class... Args>
[[nodiscard]] T *makeAs(Role role, Args &&...args) {
  return withRole(role, [&](auto r) {
    return make<T, decltype(r)::value>(std::forward<Args>(args)...);
  });
}

template <Destroyable T> void destroyAs(T *p, Role role) noexcept {
  withRole(role, [&](auto r) { destroy<T, decltype(r)::value>(p); });
}

template <class T, Role R = roleOf<std::remove_cv_t<T>>> class Allocator {
  static_assert(!std::is_const_v<T>,
                "standard containers need a non-const value_type");

public:
  using value_type = T;
  using is_always_equal = std::true_type;
  static constexpr Role role = R;

  template <class U> struct rebind {
    using other = Allocator<U, R>;
  };

  constexpr Allocator() noexcept = default;
  template <class U> constexpr Allocator(const Allocator<U, R> &) noexcept {}

  [[nodiscard]] T *allocate(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
      throw std::bad_array_new_length();
    else
      return static_cast<T *>(alloc::allocate<R>(n * sizeof(T), alignof(T)));
  }

  void deallocate(T *p, std::size_t n) noexcept {
    alloc::deallocate<R>(p, n * sizeof(T), alignof(T));
  }

  friend constexpr bool operator==(const Allocator &,
                                   const Allocator &) noexcept {
    return true;
  }
};
static_assert(std::is_empty_v<Allocator<int>>);

template <class T, Role R = roleOf<std::remove_cv_t<T>>>
using Vector = std::vector<T, Allocator<T, R>>;
template <class T, Role R = roleOf<std::remove_cv_t<T>>>
using List = std::list<T, Allocator<T, R>>;
template <class T, Role R = roleOf<std::remove_cv_t<T>>>
using Deque = std::deque<T, Allocator<T, R>>;

template <class K, class V, Role R = Role::Default,
          class Compare = std::less<K>>
using Map = std::map<K, V, Compare, Allocator<std::pair<const K, V>, R>>;

template <class K, class V, Role R = Role::Default, class Hash = std::hash<K>,
          class Eq = std::equal_to<K>>
using UnorderedMap =
    std::unordered_map<K, V, Hash, Eq, Allocator<std::pair<const K, V>, R>>;

template <Role R = Role::Default>
using String =
    std::basic_string<char, std::char_traits<char>, Allocator<char, R>>;

// lets the backend recycle transient memory. under plain it is a noop
inline void endPhase() noexcept { Backend::endPhase(); }

// the seam's part of a record, for metrics["seam"]. leakedBytes is
// seamStats.totalLiveBytes after completing the workload, which should be 0.
inline nlohmann::ordered_json seamJson(const SeamRegion &region,
                                       std::uint64_t leakedBytes) {
  nlohmann::ordered_json j;
  j["backend"] = std::string(Backend::name);
  nlohmann::ordered_json regionJson = toJson(region);
  for (const auto &item : regionJson.items())
    j[item.key()] = item.value();
  j["leakedBytes"] = leakedBytes;
  j["backendStats"] = Backend::extraStats();
  return j;
}

} // namespace tabench::alloc
