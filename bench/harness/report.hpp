#pragma once

// The common part of every result line: what ran, how it was built, the clock,
// the timer overhead, the measured region and the parameters. A workload adds
// its own values under "metrics" and writes the line with writeRecord().
//
// Usage, after the measured region and recalibrate():
//   auto record = makeRecord("churn", "default", clock, overhead, region,
//   args); record["metrics"]["nsPerOp"] = ...; writeRecord(record,
//   args.getString("out", ""));

#include "args.hpp"
#include "clock.hpp"
#include "region.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

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
inline constexpr int resultSchema = 1;

// The allocation backend this binary was compiled for.
constexpr const char *backendName() noexcept {
#if defined(TA_BACKEND_PLAIN)
  return "plain";
#elif defined(TA_BACKEND_POOLS)
  return "pools";
#elif defined(TA_BACKEND_TCMALLOC)
  return "tcmalloc";
#else
  return "none";
#endif
}

inline Json envOrNull(const char *name) {
  const char *value = std::getenv(name);
  return value ? Json(value) : Json(nullptr);
}

// ISO 8601, e.g. "2026-10-17T02:30:00Z".
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

// Builds the record with every common field, in a fixed order. "metrics" is
// left empty for the workload; "counters" and "memory" are null until perf
// counters and memory stats exist.
inline Json makeRecord(std::string_view workload, std::string_view variant,
                       const ClockInfo &clock, const TimerOverhead &overhead,
                       const RegionResult &region, const Args &args) {
  Json j;
  j["schema"] = resultSchema;
  j["timestamp"] = utcTimestamp();
  j["workload"] = workload;
  j["variant"] = variant;
  j["backend"] = backendName();
  j["ldPreload"] = envOrNull("LD_PRELOAD");
  j["build"]["compiler"] = TA_COMPILER;
  j["build"]["buildType"] = TA_BUILD_TYPE;
  j["build"]["native"] = TA_NATIVE != 0;
  j["clock"] = toJson(clock);
  j["timerOverhead"] = toJson(overhead, clock);
  j["region"] = toJson(region, clock);
  j["params"] = args.params();
  j["metrics"] = Json::object();
  j["counters"] = nullptr;
  j["memory"] = nullptr;
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
