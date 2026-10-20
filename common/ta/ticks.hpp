#pragma once

// The CPU's fixed-rate counter, shared by the bench and the profiler. x86-64
// uses the TSC, AArch64 the generic timer's virtual counter (cntvct_el0).
// Ticks are wall time, not core cycles. Nothing here allocates, so the
// profiler can call it from its preload constructor.

#include <cstdint>
#include <ctime>
#include <limits>

#if defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#elif !defined(__aarch64__)
#error "tieralloc supports x86-64 and AArch64 only"
#endif

namespace ta {

inline std::uint64_t readTicks() noexcept {
#if defined(__x86_64__)
  _mm_lfence();
  std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#else
  std::uint64_t t;
  asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(t) : : "memory");
  return t;
#endif
}

// Counter frequency as reported by the hardware, or 0 if it does not say.
// On x86, this is leaf 0x15 when queried via cpuid.
inline std::uint64_t nominalHz() noexcept {
#if defined(__x86_64__)
  unsigned eax, ebx, ecx, edx;
  if (__get_cpuid(0x15, &eax, &ebx, &ecx, &edx) && eax && ebx && ecx)
    return std::uint64_t{ecx} * ebx / eax;
  return 0;
#else
  std::uint64_t freq;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  return freq;
#endif
}

// True if the counter runs at a constant rate, regardless of P-, C-, or
// T-states.
inline bool invariantTicks() noexcept {
#if defined(__x86_64__)
  unsigned eax, ebx, ecx, edx;
  return __get_cpuid(0x80000007, &eax, &ebx, &ecx, &edx) && (edx & (1u << 8));
#else
  return true;
#endif
}

inline std::int64_t monotonicNs() noexcept {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return std::int64_t{ts.tv_sec} * 1'000'000'000 + ts.tv_nsec;
}

struct TickSample {
  std::uint64_t ticks;
  std::int64_t ns;
};

// Pairs a counter read with a CLOCK_MONOTONIC.
inline TickSample samplePair(int tries = 16) noexcept {
  TickSample best{};
  std::uint64_t bestGap = std::numeric_limits<std::uint64_t>::max();
  for (int i = 0; i < tries; ++i) {
    std::uint64_t t0 = readTicks();
    std::int64_t ns = monotonicNs();
    std::uint64_t t1 = readTicks();
    if (t1 - t0 < bestGap) {
      bestGap = t1 - t0;
      best = {t0 + (t1 - t0) / 2, ns};
    }
  }
  return best;
}

} // namespace ta
