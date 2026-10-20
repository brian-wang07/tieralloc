#pragma once

// The common part of every result line: what ran, how it was built, the clock,
// the timer overhead, the counters, the measured region, memory and the
// parameters. A workload adds its own values under "metrics" and writes the
// line with writeRecord().
//
// Usage, after the measured region and recalibrate():
//   auto record = makeRecord("churn", "default", clock, overhead, counters,
//                            region, memory, args);
//   record["metrics"]["nsPerOp"] = ...;
//   writeRecord(record, args.getString("out", ""));

#include "allocator.hpp"
#include "args.hpp"
#include "clock.hpp"
#include "contracts.hpp"
#include "counters.hpp"
#include "memstats.hpp"
#include "perf.hpp"
#include "region.hpp"
#include "summary.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

// Defined by CMake (tieralloc_options); fallbacks for builds outside it.
#ifndef TA_COMPILER
#define TA_COMPILER "unknown"
#endif
#ifndef TA_BUILD_TYPE
#define TA_BUILD_TYPE "unknown"
#endif
#ifndef TA_NATIVE
#define TA_NATIVE 0
#endif

namespace tabench {

using Json = nlohmann::ordered_json;

// Bump whenever a field is added, removed or changes meaning.
inline constexpr int resultSchema = 5;

// The allocation backend this binary was compiled for.
constexpr const char *backendName() noexcept {
#if defined(TA_BACKEND_PLAIN)
  return "plain";
#elif defined(TA_BACKEND_POOLS)
  return "pools";
#else
  return "none";
#endif
}

inline Json envOrNull(const char *name) {
  const char *value = std::getenv(name);
  return value ? Json(value) : Json(nullptr);
}

// ISO 8601, e.g. "2026-10-17T02:30:00Z".
// The allocator tuning variables set for this run (glibc tunables, jemalloc's
// MALLOC_CONF, MIMALLOC_*, TCMALLOC_*), sorted by name. The runner applies
// them from the manifest; recording them shows what a result ran with.
inline Json allocatorEnv() {
  Json j = Json::object();
#if defined(__linux__)
  constexpr std::array<std::string_view, 4> prefixes{
      "GLIBC_TUNABLES=", "MALLOC_CONF=", "MIMALLOC_", "TCMALLOC_"};
  std::vector<std::string_view> settings;
  for (char **entry = environ; *entry != nullptr; ++entry) {
    std::string_view setting(*entry);
    for (std::string_view prefix : prefixes)
      if (setting.starts_with(prefix)) {
        settings.push_back(setting);
        break;
      }
  }
  std::ranges::sort(settings);
  for (std::string_view setting : settings) {
    const std::size_t equals = setting.find('=');
    j[std::string(setting.substr(0, equals))] =
        std::string(setting.substr(equals + 1));
  }
#endif
  return j;
}

inline std::string utcTimestamp() {
  std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

inline Json toJson(const ClockInfo &c) {
  bool recalibrated = c.measuredEnd != 0;
  Json j;
  j["source"] = c.source;
  j["invariant"] = c.invariant;
  j["nominalHz"] = c.nominalHz;
  j["ticksPerNs"] = c.ticksPerNs;
  j["measuredStart"] = c.measuredStart;
  j["measuredEnd"] = recalibrated ? Json(c.measuredEnd) : Json(nullptr);
  j["drift"] = recalibrated ? Json(c.drift()) : Json(nullptr);
  return j;
}

inline Json toJson(const TimerOverhead &o, const ClockInfo &c) {
  Json j;
  j["minTicks"] = o.minTicks;
  j["medianTicks"] = o.medianTicks;
  j["meanTicks"] = o.meanTicks;
  j["meanNs"] = o.meanTicks / c.ticksPerNs;
  return j;
}

inline Json toJson(const RegionResult &r, const ClockInfo &c) {
  auto cpuOrNull = [](int cpu) {
    return cpu == -1 ? Json(nullptr) : Json(cpu);
  };
  Json j;
  j["ticks"] = r.ticks();
  j["ns"] = r.ns(c);
  j["cpuBegin"] = cpuOrNull(r.cpuBegin);
  j["cpuEnd"] = cpuOrNull(r.cpuEnd);
  j["migrated"] = r.migrated();
  return j;
}

// Per-counter values as an object keyed by the table's event names.
inline Json counterValues(const std::array<std::uint64_t, counterCount> &values,
                          const EventTable &table) {
  Json j = Json::object();
  for (const EventSpec &spec : table.events)
    j[spec.name] = values[index(spec.counter)];
  return j;
}

inline Json toJson(const CounterResult &r, const EventTable &table) {
  Json j;
  j["status"] = toString(r.status);
  j["multiplexRatio"] = r.multiplexRatio();
  j["enabledNs"] = r.enabledNs;
  j["runningNs"] = r.runningNs;
  j["values"] = counterValues(r.values, table);
  return j;
}

// The group's setup, its empty-region overhead and the region's counts; or
// the reason there are no counters.
inline Json toJson(const Counters &counters, const RegionResult &region) {
  Json j;
  j["mode"] = toString(counters.mode);
  if (!counters.group) {
    j["available"] = false;
    j["unavailable"] = counters.unavailable;
    return j;
  }
  const PerfGroup &group = *counters.group;
  const detail::PMUInfo &pmu = group.pmuInfo();
  j["available"] = true;
  j["pmu"]["device"] = pmu.device;
  j["pmu"]["model"] = pmu.model;
  j["pmu"]["type"] = pmu.type;
  j["pmu"]["hybrid"] = pmu.hybrid;
  j["table"] = group.table().id;
  j["events"] = Json::array();
  for (const EventSpec &spec : group.table().events)
    j["events"].push_back(
        {{"name", spec.name}, {"encoding", group.encodingOf(spec)}});
  j["excludeKernel"] = true;
  j["pinned"] = true;
  j["overhead"]["ok"] = counters.overhead.ok;
  j["overhead"]["median"] =
      counterValues(counters.overhead.median, group.table());
  std::optional<CounterResult> result = region.counters();
  j["region"] = result ? toJson(*result, group.table()) : Json(nullptr);
  return j;
}

inline Json toJson(const MemoryStats &m) {
  if (!m.ok)
    return nullptr;
  Json j;
  j["rssKiB"] = m.rssKiB;
  j["anonymousKiB"] = m.anonymousKiB;
  j["anonHugePagesKiB"] = m.anonHugePagesKiB;
  j["lazyFreeKiB"] = m.lazyFreeKiB;
  j["heldAnonKiB"] = m.heldAnonKiB();
  j["vmRSSKiB"] = m.vmRSSKiB;
  j["vmHWMKiB"] = m.vmHWMKiB;
  return j;
}

inline Json toJson(const Summary &s) {
  Json j;
  j["count"] = s.count;
  j["min"] = s.min;
  j["p50"] = s.p50;
  j["p99"] = s.p99;
  j["max"] = s.max;
  j["mean"] = s.mean;
  return j;
}

// Ticks are absolute; convert differences with clock.ticksPerNs.
inline Json toJson(const RSSSample &s) {
  if (!s.ok)
    return nullptr;
  Json j;
  j["ticks"] = s.ticks;
  j["rssBytes"] = s.rssBytes;
  j["anonBytes"] = s.anonBytes;
  j["liveBytes"] = s.liveBytes;
  return j;
}

// The peak is VmHWM after the region. With the reset it is the peak during the
// region; peakGrowthKiB is how far it rose above the RSS at the reset. Both
// sides come from the kernel's RSS counters, so they are comparable.
inline Json toJson(const MemoryReport &m) {
  Json j;
  j["startup"] = toJson(m.startup);
  j["before"] = toJson(m.before);
  j["after"] = toJson(m.after);
  j["peakReset"] = m.peakReset;
  bool peakKnown = m.before.ok && m.after.ok;
  j["peakKiB"] = peakKnown ? Json(m.after.vmHWMKiB) : Json(nullptr);
  j["peakGrowthKiB"] = peakKnown && m.peakReset
                           ? Json(static_cast<std::int64_t>(m.after.vmHWMKiB) -
                                  static_cast<std::int64_t>(m.before.vmRSSKiB))
                           : Json(nullptr);
  j["inputBytes"] = m.inputBytes;
  j["samples"] = Json::array();
  for (const RSSSample &s : m.samples)
    j["samples"].push_back(toJson(s));
  return j;
}

// Builds the record with every common field, in a fixed order. "metrics" is
// left empty for the workload.
inline Json makeRecord(std::string_view workload, std::string_view variant,
                       const ClockInfo &clock, const TimerOverhead &overhead,
                       const Counters &counters, const RegionResult &region,
                       const MemoryReport &memory, const Args &args) {
  Json j;
  j["schema"] = resultSchema;
  j["timestamp"] = utcTimestamp();
  j["workload"] = workload;
  j["variant"] = variant;
  j["backend"] = backendName();
  j["ldPreload"] = envOrNull("LD_PRELOAD");
  j["allocatorEnv"] = allocatorEnv();
  AllocatorInfo allocator = detectAllocator();
  j["allocator"]["name"] = allocator.name;
  j["allocator"]["version"] = allocator.version;
  j["build"]["compiler"] = TA_COMPILER;
  j["build"]["buildType"] = TA_BUILD_TYPE;
  j["build"]["native"] = TA_NATIVE != 0;
  j["build"]["contractSemantic"] = TA_CONTRACT_SEMANTIC;
  j["clock"] = toJson(clock);
  j["timerOverhead"] = toJson(overhead, clock);
  j["region"] = toJson(region, clock);
  j["params"] = args.params();
  j["metrics"] = Json::object();
  j["counters"] = toJson(counters, region);
  j["memory"] = toJson(memory);
  return j;
}

// Writes the record as one line: to stdout if outPath is empty, otherwise
// appended to that file. Invalid UTF-8 in strings is replaced rather than
// thrown on. Throws std::runtime_error if the file cannot be written.
inline void writeRecord(const Json &record, std::string_view outPath) {
  std::string line =
      record.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
  if (outPath.empty()) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
    return;
  }
  std::string path(outPath);
  std::FILE *file = std::fopen(path.c_str(), "a");
  if (!file)
    throw std::runtime_error(std::format("cannot open '{}' for appending: {}",
                                         path, std::strerror(errno)));
  bool ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
  ok = std::fclose(file) == 0 && ok;
  if (!ok)
    throw std::runtime_error(std::format("failed to write '{}'", path));
}

} // namespace tabench
