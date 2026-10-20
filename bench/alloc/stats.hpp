#pragma once

// Live requested bytes per role, counted at the seam. This is the denominator
// of the fragmentation ratio (held RSS / live bytes).
//
// Usage around a measured region:
//   SeamRegion seam;
//   seam.begin = seamStats.beginRegion();
//   region.begin(); ... region.end();
//   seam.end = seamStats;
//   record["metrics"]["seam"] = toJson(seam);

#include "role.hpp"
#include "ta/contracts.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifndef TA_SEAM_CHECKS
#ifdef NDEBUG
#define TA_SEAM_CHECKS 0
#else
#define TA_SEAM_CHECKS 1
#endif
#endif

namespace tabench::alloc {

namespace detail {

#if TA_SEAM_CHECKS
// Its address identifies the thread. std::thread::id is not guaranteed to be
// constexpr-constructible, which SeamStats needs to stay constinit.
inline thread_local char threadTag;

inline std::uintptr_t currentThread() noexcept {
  return reinterpret_cast<std::uintptr_t>(&threadTag);
}

// Out of line so the failure path doesn't bloat every inlined call.
[[noreturn, gnu::cold, gnu::noinline]] inline void
seamCheckFailed(const char *what, Role role, std::size_t bytes) noexcept {
  std::fprintf(stderr, "tabench seam check failed: %s (role %s, %zu bytes)\n",
               what, toString(role), bytes);
  std::abort();
}
#endif

inline nlohmann::ordered_json
roleValues(const std::array<std::uint64_t, roleCount> &values) {
  nlohmann::ordered_json j = nlohmann::ordered_json::object();
  for (std::size_t i = 0; i < roleCount; ++i)
    j[toString(static_cast<Role>(i))] = values[i];
  return j;
}

inline std::array<std::uint64_t, roleCount>
difference(const std::array<std::uint64_t, roleCount> &begin,
           const std::array<std::uint64_t, roleCount> &end) {
  std::array<std::uint64_t, roleCount> d{};
  for (std::size_t i = 0; i < roleCount; ++i)
    d[i] = end[i] - begin[i];
  return d;
}

} // namespace detail

// Plain data plus the functions that keep totalLiveBytes and peakLiveBytes
// consistent; Not thread-safe
struct SeamStats {
  std::array<std::uint64_t, roleCount> liveBytes{};
  std::array<std::uint64_t, roleCount> allocations{};
  std::array<std::uint64_t, roleCount> frees{};
  std::uint64_t totalLiveBytes{0};
  std::uint64_t peakLiveBytes{0};
#if TA_SEAM_CHECKS
  std::uintptr_t ownerThread = 0; // 0 until first record
#endif

  template <Role R>
  [[gnu::always_inline]] inline void
  recordAllocate(std::size_t bytes) noexcept {
#if TA_SEAM_CHECKS
    checkThread(R, bytes);
#endif
    liveBytes[index(R)] += bytes;
    totalLiveBytes += bytes;
    ++allocations[index(R)];
    peakLiveBytes = std::max(peakLiveBytes, totalLiveBytes);
  }

  template <Role R>
  [[gnu::always_inline]] inline void recordFree(std::size_t bytes) noexcept
      TA_PRE(liveBytes[index(R)] >= bytes) {
#if TA_SEAM_CHECKS
    checkThread(R, bytes);
#endif
    liveBytes[index(R)] -= bytes;
    totalLiveBytes -= bytes;
    ++frees[index(R)];
  }

  // Starts the peak at the current live total, so warmup doesn't set it.
  SeamStats beginRegion() noexcept {
    peakLiveBytes = totalLiveBytes;
    return *this;
  }

private:
#if TA_SEAM_CHECKS
  void checkThread(Role role, std::size_t bytes) noexcept {
    std::uintptr_t thread = detail::currentThread();
    if (ownerThread == 0)
      ownerThread = thread;
    else if (ownerThread != thread)
      detail::seamCheckFailed("allocation from a second thread", role, bytes);
  }
#endif
};

struct SeamRegion {
  SeamStats begin, end;
};

inline constinit SeamStats seamStats{};

inline nlohmann::ordered_json toJson(const SeamRegion &region) {
  nlohmann::ordered_json j;
  j["checks"] = static_cast<bool>(TA_SEAM_CHECKS);
  j["allocations"] = detail::roleValues(
      detail::difference(region.begin.allocations, region.end.allocations));
  j["frees"] = detail::roleValues(
      detail::difference(region.begin.frees, region.end.frees));
  j["liveBytesBegin"] = detail::roleValues(region.begin.liveBytes);
  j["liveBytesEnd"] = detail::roleValues(region.end.liveBytes);
  j["totalLiveBytesBegin"] = region.begin.totalLiveBytes;
  j["totalLiveBytesEnd"] = region.end.totalLiveBytes;
  j["peakLiveBytes"] = region.end.peakLiveBytes;
  return j;
}

} // namespace tabench::alloc
