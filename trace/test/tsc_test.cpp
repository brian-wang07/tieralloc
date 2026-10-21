// Checks for tatrace/tsc.hpp: the unfenced read is monotonic on one CPU, the
// anchor rate is stable across windows and matches the hardware's nominal
// rate, and ticksPerNs rejects anchors that don't move forward. Also prints
// the cost of the unfenced read against the fenced one, as the first entry of
// the profiler's overhead table. Exits nonzero if any check fails.

#include "ta/testing.hpp"
#include "tatrace/tsc.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <sched.h>

namespace {

using namespace ta::test;

constexpr int monotonicReads = 1'000'000;
constexpr int costReads = 10'000'000;
constexpr auto shortWindow = std::chrono::milliseconds{50};
constexpr auto longWindow = std::chrono::milliseconds{250};
constexpr double maxRateMismatch = 1e-3;

double windowRate(std::chrono::milliseconds window) {
  ta::TickSample start = ta::samplePair();
  std::this_thread::sleep_for(window);
  return tatrace::ticksPerNs(start, ta::samplePair());
}

double relativeDifference(double a, double b) { return std::fabs(a - b) / b; }

// Pinned to the current CPU, so a decrease can't come from migrating between
// cores whose counters differ slightly.
void checkMonotonic() {
  cpu_set_t saved;
  bool pinned = false;
  if (::sched_getaffinity(0, sizeof(saved), &saved) == 0) {
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(::sched_getcpu(), &one);
    pinned = ::sched_setaffinity(0, sizeof(one), &one) == 0;
  }
  int decreases = 0;
  std::uint64_t previous = tatrace::readTsc();
  for (int i = 0; i < monotonicReads; ++i) {
    std::uint64_t now = tatrace::readTsc();
    decreases += now < previous;
    previous = now;
  }
  if (pinned)
    ::sched_setaffinity(0, sizeof(saved), &saved);
  check(decreases == 0, "monotonic", "%d decrease(s) in %d reads%s", decreases,
        monotonicReads, pinned ? "" : " (unpinned)");
}

void checkRates(const tatrace::TscInfo &info) {
  double shortRate = windowRate(shortWindow);
  double longRate = windowRate(longWindow);
  double mismatch = relativeDifference(shortRate, longRate);
  check(mismatch < maxRateMismatch, "rate",
        "%lld ms %.6f vs %lld ms %.6f ticks/ns (%.2e apart)",
        static_cast<long long>(shortWindow.count()), shortRate,
        static_cast<long long>(longWindow.count()), longRate, mismatch);

  if (info.nominalHz == 0) {
    skip("nominal", "hardware doesn't report the counter rate");
    return;
  }
  double nominal = static_cast<double>(info.nominalHz) / 1e9;
  double off = relativeDifference(longRate, nominal);
  check(off < maxRateMismatch, "nominal",
        "measured %.6f vs nominal %.6f ticks/ns (%.2e apart)", longRate,
        nominal, off);
}

void checkContract() {
  if (!contractsAbort()) {
    skip("contract", "contracts not enforced in this build");
    return;
  }
  check(abortsInChild([] {
          ta::TickSample same = ta::samplePair();
          static_cast<void>(tatrace::ticksPerNs(same, same));
        }),
        "contract", "ticksPerNs with a zero-length window aborts");
}

// Informational: printed, never failed. Converted to ns with the measured
// rate, since ticks are wall time, not core cycles.
template <class F> double nsPerRead(F read, double rate) {
  std::uint64_t sink = 0;
  std::uint64_t start = ta::readTicks();
  for (int i = 0; i < costReads; ++i)
    sink ^= read();
  std::uint64_t elapsed = ta::readTicks() - start;
  // Uses the result, so the compiler cannot discard the reads.
  asm volatile("" : : "r"(sink));
  return static_cast<double>(elapsed) / rate / costReads;
}

void printCost(double rate) {
  double unfenced = nsPerRead([] { return tatrace::readTsc(); }, rate);
  double fenced = nsPerRead([] { return ta::readTicks(); }, rate);
  std::printf(
      "INFO  cost      readTsc %.2f ns, ta::readTicks %.2f ns per read\n",
      unfenced, fenced);
}

} // namespace

int main() {
  tatrace::TscInfo info = tatrace::captureTscInfo();
  std::printf("INFO  clock     invariant=%d nominal=%llu Hz\n", info.invariant,
              static_cast<unsigned long long>(info.nominalHz));
  check(info.invariant, "invariant", "counter %s constant-rate",
        info.invariant ? "is" : "is NOT");

  checkMonotonic();
  checkRates(info);
  checkContract();

  // The whole run so far as one window, the way the trace uses its anchors.
  double rate = tatrace::ticksPerNs(info.start, ta::samplePair());
  printCost(rate);

  return ta::test::finish();
}
