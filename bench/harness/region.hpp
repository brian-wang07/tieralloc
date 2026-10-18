#pragma once

#include "clock.hpp"
#include "perf.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <type_traits>
#if defined(__linux__)
#include <sched.h>
#endif

namespace tabench {

template <class T>
concept RegisterSized =
    std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(void *);

template <class T>
  requires RegisterSized<T>
[[gnu::always_inline]] inline void doNotOptimize(T &value) {
  // Clang wraps rmw with a store/reload every invocation: reads will only
  // prevent eliding new/delete and emit no other instructions.
  asm volatile("" : "+r"(value) : : "memory");
}

template <class T> [[gnu::always_inline]] inline void doNotOptimize(T &value) {
  // Larger or non-trivially copyable objects need to be gated with +m as they
  // dont fit in GPRs.
  asm volatile("" : "+m"(value) : : "memory");
}

template <class T>
  requires RegisterSized<T>
[[gnu::always_inline]] inline void doNotOptimize(const T &value) {
  asm volatile("" : : "r"(value) : "memory");
}

template <class T>
[[gnu::always_inline]] inline void doNotOptimize(const T &value) {
  asm volatile("" : : "m"(value) : "memory");
}

[[gnu::always_inline]] inline void clobberMemory() {
  asm volatile("" : : : "memory");
}

inline int currentCPU() noexcept {
  // Linux: returns the CPU number of the calling thread
  // Everything else: returns -1.
#if defined(__linux__)
  return sched_getcpu();
#else
  return -1;
#endif
}

struct RegionResult {
  std::uint64_t startTicks{0}, endTicks{0};
  int cpuBegin{-1}, cpuEnd{-1};
  CounterSnapshot counterBegin{}, counterEnd{};
  bool hasCounters{false};

  std::uint64_t ticks() const noexcept { return endTicks - startTicks; }
  bool migrated() const noexcept {
    return cpuEnd != -1 && cpuBegin != -1 && cpuBegin != cpuEnd;
  }
  double ns(const ClockInfo &c) const { return c.toNs(ticks()); }
  std::optional<CounterResult> counters() const noexcept {
    if (!hasCounters)
      return std::nullopt;
    return diff(counterBegin, counterEnd);
  }
};

struct Region {
  // With counters, the group is read just outside the timed window at both
  // ends, so the read() syscall is not part of the measured ticks.
  explicit Region(PerfGroup *counters = nullptr) noexcept : counters(counters) {
    measured.hasCounters = counters != nullptr;
  }
  ~Region() = default;

  Region(const Region &other) = delete;
  Region &operator=(const Region &other) = delete;

  [[gnu::always_inline]] inline void begin() noexcept {
    assert(state == State::Idle);
    measured.cpuBegin = currentCPU();
    if (counters)
      measured.counterBegin = counters->snapshot();
    clobberMemory();
    measured.startTicks = readTicks();
    clobberMemory();
    state = State::Running;
  }

  [[gnu::always_inline]] inline void end() noexcept {
    assert(state == State::Running);
    clobberMemory();
    measured.endTicks = readTicks();
    clobberMemory();
    if (counters)
      measured.counterEnd = counters->snapshot();
    measured.cpuEnd = currentCPU();
    state = State::Done;
  }

  RegionResult result() const {
    assert(state == State::Done);
    return measured;
  }

private:
  enum class State { Idle, Running, Done };
  PerfGroup *counters;
  RegionResult measured;
  State state{State::Idle};
};

struct TimerOverhead {
  std::uint64_t minTicks, medianTicks;
  double meanTicks;
};

template <std::size_t N = 4096>
inline TimerOverhead measureTimerOverhead() noexcept {
  static_assert(N > 0, "N must be greater than 0");
  std::array<std::uint64_t, N> gaps;
  // warm up
  for (int i{0}; i < 500; ++i)
    readTicks();

  for (std::size_t i{0}; i < N; ++i) {
    std::uint64_t t0 = readTicks();
    clobberMemory();
    std::uint64_t t1 = readTicks();
    gaps[i] = t1 - t0;
  }

  std::nth_element(gaps.begin(), gaps.begin() + N / 2, gaps.end());
  return {*std::min_element(gaps.begin(), gaps.end()), gaps[N / 2],
          static_cast<double>(
              std::accumulate(gaps.begin(), gaps.end(), std::uint64_t{0})) /
              N};
}

// Runs empty regions and keeps the median count per counter.
template <std::size_t N = 255>
inline CounterOverhead measureCounterOverhead(PerfGroup &group) noexcept {
  static_assert(N > 0, "N must be greater than 0");
  std::array<std::array<std::uint64_t, N>, counterCount> samples;
  CounterOverhead overhead;
  overhead.ok = true;
  for (std::size_t i{0}; i < N; ++i) {
    Region region(&group);
    region.begin();
    region.end();
    CounterResult r = *region.result().counters();
    overhead.ok = overhead.ok && r.status == CounterStatus::Ok;
    for (std::size_t c{0}; c < counterCount; ++c)
      samples[c][i] = r.values[c];
  }
  for (std::size_t c{0}; c < counterCount; ++c) {
    std::nth_element(samples[c].begin(), samples[c].begin() + N / 2,
                     samples[c].end());
    overhead.median[c] = samples[c][N / 2];
  }
  return overhead;
}

template <class Fn>
void timeBatches(Fn &&fn, std::size_t opsPerBatch, std::span<std::uint64_t> out,
                 std::size_t warmup = 2) {
  for (std::size_t batch{0}; batch < warmup + out.size(); ++batch) {
    clobberMemory();
    std::uint64_t t0 = readTicks();
    for (std::size_t i{0}; i < opsPerBatch; ++i) {
      std::size_t opIndex = batch * opsPerBatch + i;
      fn(opIndex);
    }
    clobberMemory();
    std::uint64_t t1 = readTicks();
    clobberMemory();
    if (batch >= warmup)
      out[batch - warmup] = t1 - t0;
  }
}

} // namespace tabench
