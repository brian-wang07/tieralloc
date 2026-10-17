// Harness self-test. Run it first on any new machine: it checks that the
// counter is usable, that regions and batches time correctly, that the thread
// stayed on one CPU, and that argument parsing behaves. Exits nonzero if any
// check fails.
//
// On Linux, run it pinned (e.g. `taskset -c 2 ./harness-selftest`); unpinned,
// the pinning check can fail legitimately.

#include "harness/args.hpp"
#include "harness/region.hpp"
#include "harness/report.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

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

int failures = 0;

[[gnu::format(printf, 3, 4)]] void check(bool ok, const char *name,
                                         const char *fmt, ...) {
  std::printf("%s  %-9s ", ok ? "PASS" : "FAIL", name);
  va_list args;
  va_start(args, fmt);
  std::vprintf(fmt, args);
  va_end(args);
  std::printf("\n");
  if (!ok)
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

// Builds a record from this run's real values, writes it to a temp file and
// reads it back.
void checkReport(const tabench::ClockInfo &clock,
                 const tabench::TimerOverhead &overhead,
                 const tabench::RegionResult &region) {
  const char *argv[] = {"prog", "--ops=3"};
  tabench::Args args(2, argv);
  args.getU64("ops", 1);
  tabench::Json record =
      tabench::makeRecord("selftest", "default", clock, overhead, region, args);
  record["metrics"]["nanValue"] = std::nan("");
  record["metrics"]["text"] = "quote\" tab\t";

  std::vector<std::string> keys;
  for (const auto &item : record.items())
    keys.push_back(item.key());
  const std::vector<std::string> expectedKeys = {
      "schema",    "timestamp", "workload", "variant",       "backend",
      "ldPreload", "build",     "clock",    "timerOverhead", "region",
      "params",    "metrics",   "counters", "memory"};
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
  bool roundTrip = !parsed.is_discarded() && singleLine &&
                   parsed["metrics"]["nanValue"].is_null() &&
                   parsed["metrics"]["text"] == "quote\" tab\t" &&
                   parsed["params"] == args.params() &&
                   parsed["region"]["ticks"] == region.ticks() &&
                   parsed["backend"] == "none";
  check(roundTrip, "report", "written line parses back (%zu bytes)",
        line.size());
}

} // namespace

int main() {
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
  check(sleepMs >= minSleepMs && sleepMs <= maxSleepMs, "sleep",
        "100 ms sleep measured %.2f ms", sleepMs);
  if (sleepResult.cpuBegin == -1)
    check(true, "pinning", "CPU unknown on this platform (not checked)");
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
  check(ratio >= minScaling && ratio <= maxScaling, "scaling",
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

  tabench::recalibrate(clock);
  check(std::abs(clock.drift()) <= maxDrift, "drift",
        "start/end rate differ by %.2e", clock.drift());

  checkReport(clock, overhead, sleepResult);

  std::printf("%s: %d check(s) failed\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
