// Harness self-test. Run it first on any new machine: it checks that the
// counter is usable, that regions and batches time correctly, that the thread
// stayed on one CPU, and that argument parsing behaves. Exits nonzero if any
// check fails.
//
// On Linux, run it pinned (e.g. `taskset -c 2 ./harness-selftest`); unpinned
// (as under ctest), the pinning check is skipped. Counter checks are skipped
// when no counters can be opened (VMs, CI) unless --require-counters is given;
// --counter-table=auto|raw|generic picks the event table to check.
// --no-timing skips the wall-clock checks.
// --expect-allocator=NAME fails unless that allocator serves malloc; the
// allocator build script uses it to check each preloaded library.

#include "harness/allocator.hpp"
#include "harness/args.hpp"
#include "harness/counters.hpp"
#include "harness/memstats.hpp"
#include "harness/perf.hpp"
#include "harness/region.hpp"
#include "harness/report.hpp"
#include "harness/summary.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#include <sys/mman.h>
#endif

namespace {

// How many CPUs this thread may run on, or 0 if unknown. Migration only
// means something when the affinity mask holds a single CPU.
int allowedCPUs() {
#if defined(__linux__)
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof(set), &set) == 0)
    return CPU_COUNT(&set);
#endif
  return 0;
}

// Thresholds. The sleep bound is loose because the OS may oversleep by a few
// ms; the scaling band is loose because macOS cannot pin threads.
constexpr double maxDrift = 1e-4;
constexpr double minSleepMs = 99.5;
constexpr double maxSleepMs = 120.0;
constexpr double minScaling = 1.7;
constexpr double maxScaling = 2.3;
constexpr std::size_t baseOpsPerBatch = 200'000;
constexpr std::size_t batchCount = 9;
constexpr double spinMs = 200.0;
// Counter checks. Instruction counts are exact up to the region's own
// bookkeeping, so the scaling band is tight.
constexpr double minInstructionScaling = 1.98;
constexpr double maxInstructionScaling = 2.02;
constexpr std::size_t largeChaseBytes = std::size_t{128} << 20;
constexpr std::size_t smallChaseBytes = std::size_t{16} << 10;
constexpr std::size_t chaseHops = 1'000'000;
constexpr double minMissesPerHop = 0.5;
constexpr double maxHitsMissesPerHop = 0.01;
// Memory checks, in KiB. The bands allow for the process's own allocations
// and for the kernel's per-CPU RSS counters lagging slightly.
constexpr std::uint64_t mib = 1024;
constexpr std::size_t anonTestBytes = std::size_t{64} << 20;
constexpr std::int64_t minAnonGrowthKiB = 60 * mib;
constexpr std::int64_t maxAnonGrowthKiB = 68 * mib;
constexpr std::int64_t maxReleaseSlackKiB = 4 * mib;
constexpr std::size_t lazyFreeTestBytes = std::size_t{16} << 20;
constexpr std::int64_t minLazyFreeKiB = 15 * mib;
constexpr std::int64_t maxRSSDriftKiB = 1 * mib;
constexpr std::size_t hugeTestBytes = std::size_t{8} << 20;
constexpr std::size_t hugePageBytes = std::size_t{2} << 20;
constexpr std::int64_t minHugeKiB = 2 * mib;

int failures = 0;
// Set by --no-timing: wall-clock checks report SKIP instead of failing. For
// smoke tests on a loaded machine, where only correctness is in question.
bool skipTiming = false;

void report(const char *status, const char *name, const char *fmt,
            va_list args) {
  std::printf("%s  %-9s ", status, name);
  std::vprintf(fmt, args);
  std::printf("\n");
}

[[gnu::format(printf, 3, 4)]] void check(bool ok, const char *name,
                                         const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  report(ok ? "PASS" : "FAIL", name, fmt, args);
  va_end(args);
  if (!ok)
    ++failures;
}

// A check that depends on wall-clock stability.
[[gnu::format(printf, 3, 4)]] void checkTiming(bool ok, const char *name,
                                               const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  report(skipTiming ? "SKIP" : ok ? "PASS" : "FAIL", name, fmt, args);
  va_end(args);
  if (!ok && !skipTiming)
    ++failures;
}

std::uint64_t median(std::vector<std::uint64_t> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

// Keeps the core busy so it ramps to full clock speed before timing; after a
// sleep it may still be running slow.
void spinFor(const tabench::ClockInfo &clock, double ms) {
  std::uint64_t start = tabench::readTicks();
  while (clock.toNs(tabench::readTicks() - start) < ms * 1e6)
    tabench::clobberMemory();
}

// The ArgError message fn throws, or "" if it throws nothing.
template <class Fn> std::string argErrorFrom(Fn &&fn) {
  try {
    fn();
  } catch (const tabench::ArgError &e) {
    return e.what();
  }
  return "";
}

void checkSummary() {
  std::vector<double> one{7.0};
  tabench::Summary s = tabench::summarize(one);
  check(s.count == 1 && s.min == 7.0 && s.p50 == 7.0 && s.p99 == 7.0 &&
            s.max == 7.0 && s.mean == 7.0,
        "summary", "a single value is every statistic");

  // Unsorted on purpose: summarize sorts.
  std::vector<double> four{3.0, 1.0, 4.0, 2.0};
  s = tabench::summarize(four);
  check(s.min == 1.0 && s.p50 == 2.5 && s.max == 4.0 && s.mean == 2.5 &&
            std::ranges::is_sorted(four),
        "summary", "p50 of an even count is the midpoint; values end sorted");

  // Rank 0.99 * 99 = 98.01 lies between 99 and 100.
  std::vector<double> hundred(100);
  std::iota(hundred.rbegin(), hundred.rend(), 1.0);
  s = tabench::summarize(hundred);
  check(std::abs(s.p99 - 99.01) < 1e-9 && s.p50 == 50.5 && s.mean == 50.5,
        "summary", "p99 of 1..100 interpolates to %.2f", s.p99);
}

void checkArgs() {
  using enum tabench::ParseError;

  // Values, defaults, flags, and what gets recorded.
  const char *argv[] = {"prog",     "--ops=100", "--rate=2.5",
                        "--name=x", "--flag",    "--typo=1"};
  tabench::Args args(static_cast<int>(std::size(argv)), argv);
  bool values =
      args.getU64("ops", 5) == 100 && args.getU64("missing", 7) == 7 &&
      args.getDouble("rate", 0) == 2.5 && args.getString("name", "") == "x" &&
      args.getFlag("flag") && !args.getFlag("absent");
  check(values, "args", "values, defaults and flags read correctly");

  std::vector<std::string_view> unused = args.unusedKeys();
  check(unused.size() == 1 && unused[0] == "typo", "args",
        "%zu unused key(s), expected just --typo", unused.size());

  std::string params = args.params().dump();
  check(params == R"({"ops":100,"missing":7,"rate":2.5,"name":"x",)"
                  R"("flag":true,"absent":false})",
        "args", "params recorded in read order: %s", params.c_str());

  // Parse helpers: every failure reason, plus the extremes that must pass.
  struct Case {
    const char *text;
    tabench::ParseError error;
  };
  const Case u64Cases[] = {{"", Empty},
                           {"abc", NotANumber},
                           {"-1", NotANumber},
                           {"+1", NotANumber},
                           {"10x", TrailingCharacters},
                           {"18446744073709551616", OutOfRange}};
  bool u64Ok = tabench::parseU64("0") == 0u &&
               tabench::parseU64("18446744073709551615") ==
                   std::numeric_limits<std::uint64_t>::max();
  for (const Case &c : u64Cases) {
    auto parsed = tabench::parseU64(c.text);
    u64Ok = u64Ok && !parsed && parsed.error() == c.error;
  }
  check(u64Ok, "parse", "parseU64 accepts valid input, rejects bad input");

  const Case doubleCases[] = {
      {"", Empty},          {" 1", NotANumber},           {"x", NotANumber},
      {"nan", NotANumber},  {"1.5x", TrailingCharacters}, {"inf", OutOfRange},
      {"1e400", OutOfRange}};
  bool doubleOk = tabench::parseDouble("2.5e3") == 2500.0 &&
                  tabench::parseDouble("-0.25") == -0.25;
  for (const Case &c : doubleCases) {
    auto parsed = tabench::parseDouble(c.text);
    doubleOk = doubleOk && !parsed && parsed.error() == c.error;
  }
  check(doubleOk, "parse",
        "parseDouble accepts valid input, rejects bad input");

  // Each error path throws ArgError with the documented message.
  struct ErrorCase {
    std::string expected;
    std::string actual;
  };
  const ErrorCase errorCases[] = {
      {"unknown argument --typo (known: --ops, --missing, --rate, --name, "
       "--flag, --absent)",
       argErrorFrom([&] { args.checkAllUsed(); })},
      {"--ops: '10x' has trailing characters", argErrorFrom([] {
         const char *bad[] = {"prog", "--ops=10x"};
         tabench::Args(2, bad).getU64("ops", 1);
       })},
      {"--ops needs a value (--ops=...)", argErrorFrom([] {
         const char *bad[] = {"prog", "--ops"};
         tabench::Args(2, bad).getU64("ops", 1);
       })},
      {"--flag is a flag and takes no value", argErrorFrom([] {
         const char *bad[] = {"prog", "--flag=1"};
         tabench::Args(2, bad).getFlag("flag");
       })},
      {"--seed given more than once", argErrorFrom([] {
         const char *bad[] = {"prog", "--seed=1", "--seed=2"};
         tabench::Args(3, bad);
       })},
      {"expected --key or --key=value, got 'ops=1'", argErrorFrom([] {
         const char *bad[] = {"prog", "ops=1"};
         tabench::Args(2, bad);
       })},
      {"missing key in '--=1'", argErrorFrom([] {
         const char *bad[] = {"prog", "--=1"};
         tabench::Args(2, bad);
       })},
  };
  for (const ErrorCase &c : errorCases)
    check(c.actual == c.expected, "argerrors", "'%s'%s", c.actual.c_str(),
          c.actual == c.expected ? ""
                                 : (" expected '" + c.expected + "'").c_str());
}

#if defined(__linux__)

// Anonymous memory kept on 4 KiB pages even under THP=always, so a chase with
// one hop per page needs one page walk per hop.
class SmallPageBuffer {
public:
  explicit SmallPageBuffer(std::size_t bytes) : bytes(bytes) {
    void *p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
      throw std::runtime_error("mmap failed");
    ::madvise(p, bytes, MADV_NOHUGEPAGE);
    base = static_cast<std::byte *>(p);
  }
  SmallPageBuffer(const SmallPageBuffer &) = delete;
  SmallPageBuffer &operator=(const SmallPageBuffer &) = delete;
  ~SmallPageBuffer() { ::munmap(base, bytes); }

  std::byte *data() const noexcept { return base; }

private:
  std::byte *base;
  std::size_t bytes;
};

// Links slots of `stride` bytes into one random cycle (Sattolo's algorithm),
// so every hop lands on a slot the prefetchers cannot predict.
void linkCycle(std::byte *base, std::size_t slots, std::size_t stride) {
  std::vector<std::uint64_t> next(slots);
  std::iota(next.begin(), next.end(), std::uint64_t{0});
  std::mt19937_64 rng(42);
  for (std::size_t i = slots - 1; i > 0; --i) {
    std::uniform_int_distribution<std::size_t> pick(0, i - 1);
    std::swap(next[i], next[pick(rng)]);
  }
  for (std::size_t i = 0; i < slots; ++i)
    *reinterpret_cast<std::uint64_t *>(base + i * stride) = next[i] * stride;
}

tabench::CounterResult countChase(tabench::PerfGroup &group,
                                  const std::byte *base) {
  std::uint64_t at = 0;
  tabench::Region region(&group);
  region.begin();
  for (std::size_t i = 0; i < chaseHops; ++i)
    at = *reinterpret_cast<const std::uint64_t *>(base + at);
  region.end();
  tabench::doNotOptimize(at);
  return *region.result().counters();
}

double perHop(const tabench::CounterResult &r, tabench::Counter c) {
  return static_cast<double>(r.value(c)) / chaseHops;
}

// Opens the largest group of generic L1D-miss events the PMU accepts. Next to
// the main group it cannot be scheduled, which exercises the detection.
struct HogGroup {
  std::array<int, tabench::detail::maxGroupEvents> fds;
  std::array<std::uint64_t, tabench::detail::maxGroupEvents> ids{};
  std::size_t size{0};

  explicit HogGroup(const tabench::detail::PMUInfo &pmu) {
    fds.fill(-1);
    tabench::EventSpec hog =
        tabench::genericEvents
            .events[tabench::index(tabench::Counter::L1DLoadMisses)];
    std::array<tabench::EventSpec, tabench::detail::maxGroupEvents> specs;
    specs.fill(hog);
    for (std::size_t n = specs.size(); n > 0 && size == 0; --n) {
      auto opened = tabench::detail::openGroup(std::span(specs).first(n), pmu,
                                               std::span(fds).first(n),
                                               std::span(ids).first(n));
      if (opened)
        size = n;
    }
  }
  HogGroup(const HogGroup &) = delete;
  HogGroup &operator=(const HogGroup &) = delete;
  ~HogGroup() { tabench::detail::closeAll(std::span(fds).first(size)); }

  tabench::CounterSnapshot snapshot() const noexcept {
    // Only the times matter; the values do not fit a CounterSnapshot.
    std::array<std::uint64_t, tabench::detail::maxGroupEvents> values{};
    tabench::CounterSnapshot s;
    s.ok = tabench::detail::readGroup(fds[0], std::span(ids).first(size),
                                      std::span(values).first(size),
                                      s.timeEnabled, s.timeRunning);
    return s;
  }
};

void checkCounters(const tabench::ClockInfo &clock, bool required,
                   tabench::TableChoice table) {
  using tabench::Counter;
  using tabench::CounterStatus;

  auto opened = tabench::PerfGroup::open(table);
  if (!opened) {
    if (required)
      check(false, "counters", "%s", opened.error().c_str());
    else
      std::printf("SKIP  counters  %s\n", opened.error().c_str());
    return;
  }
  tabench::PerfGroup &group = *opened;
  const tabench::detail::PMUInfo &pmu = group.pmuInfo();
  check(true, "counters", "opened table %s on %s (%s, type %u)",
        group.table().id, pmu.device.empty() ? "default" : pmu.device.c_str(),
        pmu.model.empty() ? "unknown model" : pmu.model.c_str(), pmu.type);

  tabench::CounterOverhead overhead = tabench::measureCounterOverhead(group);
  std::printf("INFO  overhead  empty region: %llu cycles, %llu instructions\n",
              static_cast<unsigned long long>(
                  overhead.median[tabench::index(Counter::Cycles)]),
              static_cast<unsigned long long>(
                  overhead.median[tabench::index(Counter::Instructions)]));

  // Twice the ops should retire twice the instructions, unmultiplexed.
  std::uint64_t acc = 1;
  auto countOps = [&](std::size_t ops) {
    tabench::Region region(&group);
    region.begin();
    for (std::size_t i = 0; i < ops; ++i) {
      acc = acc * 6364136223846793005ULL + i;
      tabench::doNotOptimize(acc);
    }
    region.end();
    return *region.result().counters();
  };
  spinFor(clock, spinMs);
  tabench::CounterResult single = countOps(baseOpsPerBatch);
  tabench::CounterResult doubled = countOps(2 * baseOpsPerBatch);
  check(single.status == CounterStatus::Ok &&
            doubled.status == CounterStatus::Ok,
        "multiplex", "status %s/%s, running/enabled %.4f/%.4f",
        tabench::toString(single.status), tabench::toString(doubled.status),
        single.multiplexRatio(), doubled.multiplexRatio());
  double ratio = static_cast<double>(doubled.value(Counter::Instructions)) /
                 static_cast<double>(single.value(Counter::Instructions));
  check(ratio >= minInstructionScaling && ratio <= maxInstructionScaling &&
            single.value(Counter::Cycles) > 0,
        "instrs", "2x ops retired %.4fx instructions (%llu -> %llu)", ratio,
        static_cast<unsigned long long>(single.value(Counter::Instructions)),
        static_cast<unsigned long long>(doubled.value(Counter::Instructions)));

  // A random chase with one hop per 4 KiB page over far more than the L3 and
  // the STLB reach misses everywhere; within 16 KiB it hits L1 every time.
  // This checks the encodings count what they claim.
  SmallPageBuffer large(largeChaseBytes);
  linkCycle(large.data(), largeChaseBytes / 4096, 4096);
  SmallPageBuffer small(smallChaseBytes);
  linkCycle(small.data(), smallChaseBytes / 64, 64);
  countChase(group, small.data()); // Warm the small buffer into L1.
  tabench::CounterResult far = countChase(group, large.data());
  tabench::CounterResult near = countChase(group, small.data());
  struct MissCheck {
    Counter counter;
    const char *name;
  };
  const MissCheck missChecks[] = {{Counter::L1DLoadMisses, "l1d"},
                                  {Counter::LLCLoadMisses, "llc"},
                                  {Counter::DTLBLoadWalks, "dtlb"}};
  for (const MissCheck &m : missChecks) {
    double farRate = perHop(far, m.counter);
    double nearRate = perHop(near, m.counter);
    check(far.status == CounterStatus::Ok && near.status == CounterStatus::Ok &&
              farRate >= minMissesPerHop && nearRate <= maxHitsMissesPerHop,
          m.name, "%.3f per hop over 128 MiB, %.4f per hop within 16 KiB",
          farRate, nearRate);
  }

  // The detection itself: a second pinned group that takes every counter the
  // PMU will give it cannot run next to the main one, so one of the two must
  // report something other than Ok.
  HogGroup hog(pmu);
  if (hog.size == 0) {
    std::printf("SKIP  detect    could not open a hog group\n");
    return;
  }
  tabench::CounterSnapshot hogBegin = hog.snapshot();
  tabench::CounterResult mainResult = countOps(baseOpsPerBatch);
  tabench::CounterResult hogResult = tabench::diff(hogBegin, hog.snapshot());
  check(mainResult.status != CounterStatus::Ok ||
            hogResult.status != CounterStatus::Ok,
        "detect", "with a %zu-event hog group: main %s, hog %s", hog.size,
        tabench::toString(mainResult.status),
        tabench::toString(hogResult.status));
}

void touchPages(std::byte *p, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; i += 4096)
    p[i] = std::byte{1};
  tabench::clobberMemory();
}

// Anonymous memory aligned to 2 MiB and advised for THP.
class HugePageBuffer {
public:
  explicit HugePageBuffer(std::size_t bytes)
      : mappedBytes(bytes + hugePageBytes) {
    void *p = ::mmap(nullptr, mappedBytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
      throw std::runtime_error("mmap failed");
    mapped = static_cast<std::byte *>(p);
    auto address = reinterpret_cast<std::uintptr_t>(mapped);
    base = mapped + ((hugePageBytes - address % hugePageBytes) % hugePageBytes);
    ::madvise(base, bytes, MADV_HUGEPAGE);
  }
  HugePageBuffer(const HugePageBuffer &) = delete;
  HugePageBuffer &operator=(const HugePageBuffer &) = delete;
  ~HugePageBuffer() { ::munmap(mapped, mappedBytes); }

  std::byte *data() const noexcept { return base; }

private:
  std::byte *mapped;
  std::byte *base;
  std::size_t mappedBytes;
};

std::int64_t delta(std::uint64_t to, std::uint64_t from) {
  return static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
}

// The bracketed mode in /sys/kernel/mm/transparent_hugepage/enabled.
std::string thpMode() {
  std::ifstream in("/sys/kernel/mm/transparent_hugepage/enabled");
  std::string line;
  std::getline(in, line);
  std::size_t open = line.find('['), close = line.find(']');
  if (open == std::string::npos || close == std::string::npos || close < open)
    return "";
  return line.substr(open + 1, close - open - 1);
}

void checkMemory() {
  tabench::MemoryStats start = tabench::readMemoryStats();
  check(start.ok && start.rssKiB > 0 && start.vmRSSKiB > 0, "memstats",
        "rss %llu KiB, anonymous %llu KiB, held %llu KiB",
        static_cast<unsigned long long>(start.rssKiB),
        static_cast<unsigned long long>(start.anonymousKiB),
        static_cast<unsigned long long>(start.heldAnonKiB()));
  if (!start.ok)
    return;

  // Touching 64 MiB on small pages raises Anonymous by 64 MiB; unmapping it
  // gives it back, but the peak keeps it until the reset.
  tabench::MemoryStats before = tabench::readMemoryStats();
  std::int64_t grown;
  {
    SmallPageBuffer buffer(anonTestBytes);
    touchPages(buffer.data(), anonTestBytes);
    grown = delta(tabench::readMemoryStats().anonymousKiB, before.anonymousKiB);
  }
  tabench::MemoryStats released = tabench::readMemoryStats();
  std::int64_t left = delta(released.anonymousKiB, before.anonymousKiB);
  check(grown >= minAnonGrowthKiB && grown <= maxAnonGrowthKiB &&
            left <= maxReleaseSlackKiB,
        "anon", "64 MiB touched: %+lld KiB, %+lld KiB after munmap",
        static_cast<long long>(grown), static_cast<long long>(left));

  bool reset = tabench::resetPeakRSS();
  tabench::MemoryStats afterReset = tabench::readMemoryStats();
  bool peakHeld = delta(released.vmHWMKiB, before.vmRSSKiB) >= minAnonGrowthKiB;
  bool peakDropped =
      delta(afterReset.vmHWMKiB, afterReset.vmRSSKiB) <= maxReleaseSlackKiB;
  check(reset && peakHeld && peakDropped, "peak",
        "VmHWM %llu KiB, %llu KiB after reset (VmRSS %llu KiB)%s",
        static_cast<unsigned long long>(released.vmHWMKiB),
        static_cast<unsigned long long>(afterReset.vmHWMKiB),
        static_cast<unsigned long long>(afterReset.vmRSSKiB),
        reset ? "" : ", clear_refs write refused");

  // MADV_FREE leaves pages in Rss until reclaim; LazyFree is what separates
  // them out of the held figure.
  {
    SmallPageBuffer buffer(lazyFreeTestBytes);
    touchPages(buffer.data(), lazyFreeTestBytes);
    tabench::MemoryStats touched = tabench::readMemoryStats();
    ::madvise(buffer.data(), lazyFreeTestBytes, MADV_FREE);
    tabench::MemoryStats freed = tabench::readMemoryStats();
    std::int64_t lazy = delta(freed.lazyFreeKiB, touched.lazyFreeKiB);
    std::int64_t rss = delta(freed.rssKiB, touched.rssKiB);
    std::int64_t held = delta(touched.heldAnonKiB(), freed.heldAnonKiB());
    check(lazy >= minLazyFreeKiB && std::abs(rss) <= maxRSSDriftKiB &&
              held >= minLazyFreeKiB,
          "lazyfree",
          "MADV_FREE of 16 MiB: LazyFree %+lld, Rss %+lld, "
          "held %+lld KiB",
          static_cast<long long>(lazy), static_cast<long long>(rss),
          static_cast<long long>(-held));
  }

  std::string mode = thpMode();
  if (mode.empty() || mode == "never") {
    std::printf("SKIP  thp       transparent huge pages are %s\n",
                mode.empty() ? "unavailable" : "disabled");
  } else {
    tabench::MemoryStats plain = tabench::readMemoryStats();
    HugePageBuffer buffer(hugeTestBytes);
    touchPages(buffer.data(), hugeTestBytes);
    std::int64_t huge = delta(tabench::readMemoryStats().anonHugePagesKiB,
                              plain.anonHugePagesKiB);
    check(huge >= minHugeKiB, "thp",
          "8 MiB with MADV_HUGEPAGE (mode %s): AnonHugePages %+lld KiB",
          mode.c_str(), static_cast<long long>(huge));
  }

  // The cheap statm sample agrees with the full reading.
  tabench::RSSSample sample = tabench::sampleRSS();
  tabench::MemoryStats now = tabench::readMemoryStats();
  std::int64_t rssGap = delta(sample.rssBytes / 1024, now.vmRSSKiB);
  std::int64_t anonGap = delta(sample.anonBytes / 1024, now.anonymousKiB);
  std::int64_t allowed = std::max<std::int64_t>(
      maxRSSDriftKiB, static_cast<std::int64_t>(now.vmRSSKiB / 20));
  check(sample.ok && std::abs(rssGap) <= allowed &&
            std::abs(anonGap) <= allowed,
        "statm", "sample vs full reading: rss %+lld KiB, anon %+lld KiB",
        static_cast<long long>(rssGap), static_cast<long long>(anonGap));
}

#endif // __linux__

// Builds a record from this run's real values, writes it to a temp file and
// reads it back.
void checkReport(const tabench::ClockInfo &clock,
                 const tabench::TimerOverhead &overhead,
                 const tabench::Counters &counters,
                 const tabench::RegionResult &region,
                 const tabench::MemoryReport &memory) {
  const char *argv[] = {"prog", "--ops=3"};
  tabench::Args args(2, argv);
  args.getU64("ops", 1);
  tabench::Json record = tabench::makeRecord(
      "selftest", "default", clock, overhead, counters, region, memory, args);
  record["metrics"]["nanValue"] = std::nan("");
  record["metrics"]["text"] = "quote\" tab\t";

  std::vector<std::string> keys;
  for (const auto &item : record.items())
    keys.push_back(item.key());
  const std::vector<std::string> expectedKeys = {
      "schema",  "timestamp", "workload",      "variant",
      "backend", "ldPreload", "allocatorEnv",  "allocator",
      "build",   "clock",     "timerOverhead", "region",
      "params",  "metrics",   "counters",      "memory"};
  check(keys == expectedKeys, "report", "common fields present, in order");

  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      std::format("tabench-selftest-{}.jsonl", ::getpid());
  std::filesystem::remove(path);
  tabench::writeRecord(record, path.string());
  std::ifstream in(path);
  std::string line, extra;
  std::getline(in, line);
  bool singleLine = !std::getline(in, extra);
  in.close();
  std::filesystem::remove(path);

  tabench::Json parsed = tabench::Json::parse(line, nullptr, false);
  bool roundTrip =
      !parsed.is_discarded() && singleLine &&
      parsed["metrics"]["nanValue"].is_null() &&
      parsed["metrics"]["text"] == "quote\" tab\t" &&
      parsed["params"] == args.params() &&
      parsed["region"]["ticks"] == region.ticks() &&
      parsed["backend"] == "none" &&
      parsed["counters"]["available"] == counters.group.has_value() &&
      parsed["memory"]["after"].is_null() == !memory.after.ok &&
      parsed["memory"]["samples"].size() == memory.samples.size();
  check(roundTrip, "report", "written line parses back (%zu bytes)",
        line.size());
}

} // namespace

int main(int argc, char **argv) {
  // First, so the baseline excludes everything the selftest allocates.
  tabench::MemoryReport memory;
  memory.startup = tabench::readMemoryStats();

  tabench::Args cli(argc, argv);
  bool requireCounters = cli.getFlag("require-counters");
  std::string_view tableName = cli.getString("counter-table", "auto");
  std::string_view expectedAllocator = cli.getString("expect-allocator", "");
  skipTiming = cli.getFlag("no-timing");
  cli.checkAllUsed();
  std::optional<tabench::TableChoice> table =
      tabench::parseTableChoice(tableName);
  if (!table) {
    std::fprintf(stderr, "--counter-table must be auto, raw or generic\n");
    return 2;
  }

  tabench::ClockInfo clock = tabench::calibrate();
  std::printf("INFO  clock     source=%s nominal=%llu Hz ticks/ns=%.6f\n",
              clock.source, static_cast<unsigned long long>(clock.nominalHz),
              clock.ticksPerNs);
  check(clock.invariant, "invariant", "counter %s constant-rate",
        clock.invariant ? "is" : "is NOT");

  // A region around a sleep measures wall time, and the CPU at both ends.
  tabench::Region sleepRegion;
  sleepRegion.begin();
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  sleepRegion.end();
  tabench::RegionResult sleepResult = sleepRegion.result();
  double sleepMs = sleepResult.ns(clock) / 1e6;
  checkTiming(sleepMs >= minSleepMs && sleepMs <= maxSleepMs, "sleep",
              "100 ms sleep measured %.2f ms", sleepMs);
  int cpus = allowedCPUs();
  if (sleepResult.cpuBegin == -1)
    check(true, "pinning", "CPU unknown on this platform (not checked)");
  else if (cpus != 1)
    std::printf("SKIP  pinning   not pinned (%d CPUs allowed); use taskset\n",
                cpus);
  else
    check(!sleepResult.migrated(), "pinning", "CPU %d at begin, %d at end",
          sleepResult.cpuBegin, sleepResult.cpuEnd);

  // Twice the ops per batch should take twice the time. The op is a short
  // dependency chain the compiler cannot fold away.
  std::uint64_t acc = 1;
  auto step = [&acc](std::size_t i) {
    acc = acc * 6364136223846793005ULL + i;
    tabench::doNotOptimize(acc);
  };
  std::vector<std::uint64_t> batches(batchCount);
  spinFor(clock, spinMs);
  tabench::timeBatches(step, baseOpsPerBatch, batches);
  std::uint64_t single = median(batches);
  tabench::timeBatches(step, 2 * baseOpsPerBatch, batches);
  std::uint64_t doubled = median(batches);
  double ratio = static_cast<double>(doubled) / static_cast<double>(single);
  checkTiming(ratio >= minScaling && ratio <= maxScaling, "scaling",
              "2x ops took %.2fx time (%.3f -> %.3f ns/op)", ratio,
              clock.toNs(single) / baseOpsPerBatch,
              clock.toNs(doubled) / (2 * baseOpsPerBatch));

  tabench::TimerOverhead overhead = tabench::measureTimerOverhead();
  std::printf("INFO  overhead  min=%llu median=%llu mean=%.2f ticks "
              "(%.1f ns mean)\n",
              static_cast<unsigned long long>(overhead.minTicks),
              static_cast<unsigned long long>(overhead.medianTicks),
              overhead.meanTicks, overhead.meanTicks / clock.ticksPerNs);

  checkArgs();
  checkSummary();

  tabench::AllocatorInfo allocator = tabench::detectAllocator();
  if (expectedAllocator.empty())
    std::printf("INFO  allocator %s %s\n", allocator.name.c_str(),
                allocator.version.c_str());
  else
    check(allocator.name == expectedAllocator, "allocator",
          "%s %s serves malloc (expected %.*s)", allocator.name.c_str(),
          allocator.version.c_str(), static_cast<int>(expectedAllocator.size()),
          expectedAllocator.data());

#if defined(__linux__)
  checkCounters(clock, requireCounters, *table);
  checkMemory();
#else
  if (requireCounters)
    check(false, "counters", "perf counters are Linux only");
#endif

  tabench::recalibrate(clock);
  checkTiming(std::abs(clock.drift()) <= maxDrift, "drift",
              "start/end rate differ by %.2e", clock.drift());

  // The report check goes through openCounters(), the path workloads use.
  tabench::Counters counters =
      tabench::openCounters(tabench::CounterMode::On, *table);
  std::array<tabench::RSSSample, 2> samples;
  memory.before = tabench::readMemoryStats();
  memory.peakReset = tabench::resetPeakRSS();
  tabench::Region countedRegion(counters.get());
  countedRegion.begin();
  samples[0] = tabench::sampleRSS();
  samples[1] = tabench::sampleRSS();
  countedRegion.end();
  memory.after = tabench::readMemoryStats();
  memory.samples = samples;
  checkReport(clock, overhead, counters, countedRegion.result(), memory);

  std::printf("%s: %d check(s) failed\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
