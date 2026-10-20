#pragma once

// One measured run of a bench binary, in the only order that gives honest
// numbers: startup memory, arguments, clock and counters, CPU warmup, the
// measured region between two memory snapshots, then the record. Every
// workload's main is
//
//   return tabench::Session::run("churn", [&] {
//     tabench::Session session(argc, argv, "churn");
//     ... read the workload's parameters from session.args() ...
//     session.start();
//     ... build inputs; session.warmUpCPU(); the workload's own warmup ...
//     session.measure([&] { ... the workload, session.sampleRSS(live) ... });
//     tabench::Json record = session.record(variant);
//     record["metrics"] = ...;
//     session.write(record);
//   });

#include "args.hpp"
#include "clock.hpp"
#include "counters.hpp"
#include "memstats.hpp"
#include "perf.hpp"
#include "region.hpp"
#include "report.hpp"
#include "system.hpp"
#include "ta/contracts.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tabench {

// What warmUpCPU() reached. ghz and converged are empty without counters.
struct CPUWarmup {
  double ms{0};
  std::optional<double> ghz;
  std::optional<bool> converged;
};

inline Json toJson(const CPUWarmup &w) {
  Json j;
  j["ms"] = w.ms;
  j["ghz"] = w.ghz ? Json(*w.ghz) : Json(nullptr);
  j["converged"] = w.converged ? Json(*w.converged) : Json(nullptr);
  return j;
}

inline Json toJson(const SystemInfo &s) {
  auto orNull = [](const std::optional<std::string> &value) {
    return value ? Json(*value) : Json(nullptr);
  };
  Json j;
  j["cpuModel"] = orNull(s.cpuModel);
  j["cpu"] = s.cpu == -1 ? Json(nullptr) : Json(s.cpu);
  j["kernel"] = orNull(s.kernel);
  j["thp"] = orNull(s.thp);
  j["thpDefrag"] = orNull(s.thpDefrag);
  j["governor"] = orNull(s.governor);
  return j;
}

class Session {
public:
  // The startup snapshot comes before parsing argv, because Args allocates and
  // the summary subtracts this baseline from heap RSS. So a Session must be
  // the first thing main creates.
  Session(int argc, const char *const *argv, std::string_view workload)
      : startup(readMemoryStats()), arguments(argc, argv), name(workload) {}

  // Regions and the record point into the session's counters and samples.
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  // The workload reads its own parameters here, all before start().
  Args &args() noexcept { return arguments; }

  // Reads the common flags, then rejects any flag nobody read, so it must
  // come after the workload's own parameters. Then calibrates the clock,
  // measures the timer overhead and opens the counters.
  void start() {
    TA_ASSERT(phase == Phase::Created);
    const std::string_view counterText = arguments.getString("counters", "on");
    const std::optional<CounterMode> mode = parseCounterMode(counterText);
    if (!mode)
      throw ArgError(std::format(
          "--counters must be off, on or required (got '{}')", counterText));
    const std::string_view tableText =
        arguments.getString("counter-table", "auto");
    const std::optional<TableChoice> table = parseTableChoice(tableText);
    if (!table)
      throw ArgError(
          std::format("--counter-table must be auto, raw or generic (got '{}')",
                      tableText));
    out = arguments.getString("out", "");
    warmupMs = arguments.getDouble("warmup-ms", 200.0);
    if (!(warmupMs >= 0.0))
      throw ArgError("--warmup-ms must not be negative");
    const std::uint64_t sampleCapacity = arguments.getU64("rss-samples", 4096);
    arguments.checkAllUsed();

    clock = calibrate();
    overhead = measureTimerOverhead();
    counters = openCounters(*mode, *table);
    samples.resize(sampleCapacity);
    phase = Phase::Started;
  }

  // Spins until the core's clock is steady, so the measured region doesn't
  // start while it is still ramping up (seen as a 4x spread between runs of
  // the same cell). With counters: at least --warmup-ms, then until three
  // consecutive 10 ms windows agree on cycles per ns within 1%, capped at
  // 2 s. Without: --warmup-ms. Untimed; the workload's own warmup (caches,
  // page faults) still follows.
  void warmUpCPU() {
    TA_ASSERT(phase == Phase::Started);
    constexpr double windowMs = 10.0;
    constexpr double capMs = 2000.0;
    constexpr double tolerance = 0.01;
    const std::uint64_t start = readTicks();
    auto elapsedMs = [&] { return clock.toNs(readTicks() - start) / 1e6; };

    PerfGroup *group = counters.get();
    if (group == nullptr) {
      while (elapsedMs() < warmupMs)
        clobberMemory();
      warmup = {
          .ms = elapsedMs(), .ghz = std::nullopt, .converged = std::nullopt};
      return;
    }

    double previous = 0.0;
    int agreeing = 0;
    std::optional<double> ghz;
    while (elapsedMs() < capMs) {
      const CounterSnapshot before = group->snapshot();
      const std::uint64_t t0 = readTicks();
      while (clock.toNs(readTicks() - t0) < windowMs * 1e6)
        clobberMemory();
      const std::uint64_t t1 = readTicks();
      const CounterResult r = diff(before, group->snapshot());
      if (r.status != CounterStatus::Ok)
        continue;
      ghz = static_cast<double>(r.value(Counter::Cycles)) / clock.toNs(t1 - t0);
      const bool agrees =
          previous > 0.0 && std::abs(*ghz - previous) <= tolerance * previous;
      agreeing = agrees ? agreeing + 1 : 0;
      previous = *ghz;
      if (agreeing >= 2 && elapsedMs() >= warmupMs)
        break;
    }
    warmup = {.ms = elapsedMs(), .ghz = ghz, .converged = agreeing >= 2};
  }

  // The measured phase: reset peak RSS, memory before, the region around
  // body, memory after, recalibrate. body runs exactly once.
  template <class Body> void measure(Body &&body) {
    TA_ASSERT(phase == Phase::Started);
    memory.startup = startup;
    memory.peakReset = resetPeakRSS();
    memory.before = readMemoryStats();
    // Interrupts and context switches on this CPU during the region: the
    // first thing to check when a run is slower than its cell's others.
    const int cpu = currentCPU();
    const std::optional<std::uint64_t> irqBefore = interruptsOn(cpu);
    const ContextSwitches switchesBefore = contextSwitches();
    Region region(counters.get());
    region.begin();
    std::forward<Body>(body)();
    region.end();
    const ContextSwitches switchesAfter = contextSwitches();
    const std::optional<std::uint64_t> irqAfter = interruptsOn(cpu);
    if (irqBefore && irqAfter)
      interrupts = *irqAfter - *irqBefore;
    if (switchesBefore.ok && switchesAfter.ok)
      switches = ContextSwitches{
          switchesAfter.voluntary - switchesBefore.voluntary,
          switchesAfter.involuntary - switchesBefore.involuntary, true};
    memory.after = readMemoryStats();
    recalibrate(clock);
    measured = region.result();
    phase = Phase::Measured;
  }

  // During measure(): one RSS sample into the buffer start() reserved, so the
  // region never allocates. Once it is full, samples are counted as dropped.
  // Each call reads /proc/self/statm (a few us), so call it at phase
  // boundaries or every N operations, not per operation.
  void sampleRSS(std::uint64_t liveBytes = 0) noexcept {
    if (sampleCount < samples.size())
      samples[sampleCount++] = tabench::sampleRSS(liveBytes);
    else
      ++droppedSamples;
  }

  // Before measure(): memory the workload mapped for itself (MappedArray), so
  // the summary can subtract it from the heap's RSS.
  void addInputBytes(std::uint64_t bytes) noexcept {
    TA_ASSERT(phase != Phase::Measured);
    memory.inputBytes += bytes;
  }

  // After measure(): the common record plus "system", with "metrics" left for
  // the workload.
  Json record(std::string_view variant) {
    TA_ASSERT(phase == Phase::Measured);
    memory.samples = std::span<const RSSSample>(samples.data(), sampleCount);
    Json j = makeRecord(name, variant, clock, overhead, counters, measured,
                        memory, arguments);
    j["memory"]["droppedSamples"] = droppedSamples;
    j["region"]["interrupts"] = interrupts ? Json(*interrupts) : Json(nullptr);
    j["region"]["contextSwitches"] =
        switches.ok ? Json{{"voluntary", switches.voluntary},
                           {"involuntary", switches.involuntary}}
                    : Json(nullptr);
    j["system"] = toJson(readSystemInfo(measured.cpuBegin));
    j["system"]["warmup"] = toJson(warmup);
    return j;
  }

  void write(const Json &record) const { writeRecord(record, out); }

  const ClockInfo &clockInfo() const noexcept { return clock; }
  const RegionResult &region() const noexcept { return measured; }

  // Wraps main's body so no run ever writes a partial record. A bad flag exits
  // 2 (the matrix itself is wrong, so the runner stops); any other failure
  // exits 1 (this run failed).
  template <class Main> static int run(std::string_view program, Main &&main) {
    try {
      std::forward<Main>(main)();
      return 0;
    } catch (const ArgError &e) {
      std::fprintf(stderr, "%.*s: %s\n", static_cast<int>(program.size()),
                   program.data(), e.what());
      return 2;
    } catch (const std::exception &e) {
      std::fprintf(stderr, "%.*s: %s\n", static_cast<int>(program.size()),
                   program.data(), e.what());
      return 1;
    }
  }

private:
  enum class Phase { Created, Started, Measured };

  MemoryStats startup;
  Args arguments;
  std::string_view name;
  Phase phase{Phase::Created};

  std::string_view out;
  double warmupMs{0};
  ClockInfo clock{};
  TimerOverhead overhead{};
  Counters counters;
  CPUWarmup warmup;

  MemoryReport memory;
  RegionResult measured;
  std::vector<RSSSample> samples;
  std::size_t sampleCount{0};
  std::uint64_t droppedSamples{0};
  std::optional<std::uint64_t> interrupts;
  ContextSwitches switches;
};

} // namespace tabench
