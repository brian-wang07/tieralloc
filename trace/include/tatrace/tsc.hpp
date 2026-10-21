#pragma once

// timestamps for trace records.

#include "ta/contracts.hpp"
#include "ta/ticks.hpp"

#include <cstdint>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace tatrace {

// for the allocation hooks only; timing a region uses ta::readTicks()
[[gnu::always_inline]] inline std::uint64_t readTsc() noexcept {
#if defined(__x86_64__)
  return __rdtsc();
#else
  std::uint64_t t;
  asm volatile("mrs %0, cntvct_el0" : "=r"(t));
  return t;
#endif
}

struct TscInfo {
  bool invariant;          // false: cross-thread order is approximate
  std::uint64_t nominalHz; // 0 if the hardware doesn't report it
  ta::TickSample start;    // anchor at init
};

// safe in the preload constructor: cpuid and vDSO clock_gettime only, no
// allocation.
inline TscInfo captureTscInfo() noexcept {
  return {ta::invariantTicks(), ta::nominalHz(), ta::samplePair()};
}

inline double ticksPerNs(ta::TickSample start, ta::TickSample end) noexcept
    TA_PRE(end.ns > start.ns && end.ticks > start.ticks) {
  return static_cast<double>(end.ticks - start.ticks) /
         static_cast<double>(end.ns - start.ns);
}

} // namespace tatrace
