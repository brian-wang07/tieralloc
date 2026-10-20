#pragma once

// Bench-harness clock: conversion of the CPU's fixed-rate counter to
// nanoseconds. The counter reads themselves live in ta/ticks.hpp, shared with
// the profiler.

#include "ta/ticks.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

namespace tabench {

using ta::invariantTicks;
using ta::monotonicNs;
using ta::nominalHz;
using ta::readTicks;
using ta::samplePair;
using ta::TickSample;

// Counter rate measured against CLOCK_MONOTONIC across `window`.
inline double measureTicksPerNs(
    std::chrono::milliseconds window = std::chrono::milliseconds{100}) {
  TickSample a = samplePair();
  std::this_thread::sleep_for(window);
  TickSample b = samplePair();
  return static_cast<double>(b.ticks - a.ticks) /
         static_cast<double>(b.ns - a.ns);
}

struct ClockInfo {
  const char *source;      // "tsc" or "cntvct"
  bool invariant;          // see invariantTicks()
  std::uint64_t nominalHz; // hardware-reported rate, 0 if unknown
  double ticksPerNs;       // rate used for conversions
  double measuredStart;    // measured rate at startup
  double measuredEnd;      // measured rate at exit, 0 until recalibrate()

  // For tick differences; double is exact well past any run's length.
  double toNs(std::uint64_t ticks) const noexcept {
    return static_cast<double>(ticks) / ticksPerNs;
  }

  // Relative change in the measured rate between startup and exit.
  double drift() const noexcept {
    return measuredEnd == 0 ? 0 : (measuredEnd - measuredStart) / measuredStart;
  }
};

// Call once at startup, on the pinned thread. On AArch64 the hardware rate is
// exact and used directly; on x86 the measured rate is used, since CPUID 0x15
// is often missing.
inline ClockInfo calibrate() {
  ClockInfo c{};
  c.invariant = invariantTicks();
  c.nominalHz = nominalHz();
  c.measuredStart = measureTicksPerNs();
#if defined(__x86_64__)
  c.source = "tsc";
  c.ticksPerNs = c.measuredStart;
#else
  c.source = "cntvct";
  c.ticksPerNs = static_cast<double>(c.nominalHz) / 1e9;
#endif
  return c;
}

// Call once at exit, after the measured work, to fill in drift().
inline void recalibrate(ClockInfo &c) { c.measuredEnd = measureTicksPerNs(); }

} // namespace tabench
