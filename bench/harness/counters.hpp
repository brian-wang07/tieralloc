#pragma once

// The process's counters as a bench binary uses them: the open group, or why
// there is none, plus the cost of an empty region measured once at startup.

#include "perf.hpp"
#include "region.hpp"

#include <optional>
#include <stdexcept>
#include <string>

namespace tabench {

struct Counters {
  CounterMode mode{CounterMode::Off};
  std::optional<PerfGroup> group;
  std::string unavailable; // Why group is empty.
  CounterOverhead overhead;

  // For Region; null when there is no group. Regions hold this pointer, so a
  // Counters must not move once regions use it.
  PerfGroup *get() noexcept { return group ? &*group : nullptr; }
};

// Throws std::runtime_error if mode is Required and the group cannot be
// opened, so timing runs never silently lose their counters.
inline Counters openCounters(CounterMode mode, TableChoice table) {
  Counters counters;
  counters.mode = mode;
  if (mode == CounterMode::Off) {
    counters.unavailable = "disabled by --counters=off";
    return counters;
  }
  auto opened = PerfGroup::open(table);
  if (!opened) {
    if (mode == CounterMode::Required)
      throw std::runtime_error("perf counters required: " + opened.error());
    counters.unavailable = std::move(opened.error());
    return counters;
  }
  counters.group.emplace(std::move(*opened));
  counters.overhead = measureCounterOverhead(*counters.group);
  return counters;
}

} // namespace tabench
