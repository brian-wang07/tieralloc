#pragma once

// Hardware performance counters for bench regions: one pinned perf_event group
// per process counting cycles, instructions, L1D and LLC load misses and dTLB
// load page walks, in user mode only. Values are never scaled; an interval that
// was multiplexed or not scheduled is reported as such and rejected later.
//
// Linux only. Elsewhere the types still exist and PerfGroup::open() fails with
// a reason, so the bench builds on every platform.

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace tabench {

// One slot per measured event. The values index every per-counter array, and
// Cycles comes first because index 0 is the group leader.
enum class Counter : std::uint8_t {
  Cycles,
  Instructions,
  L1DLoadMisses,
  LLCLoadMisses,
  DTLBLoadWalks
};
inline constexpr std::size_t counterCount = 5;

constexpr std::size_t index(Counter c) noexcept {
  return std::to_underlying(c);
}

// How EventSpec::config is interpreted.
enum class EventKind {
  Hardware,      // A PERF_COUNT_HW_* id.
  HardwareCache, // cache | op << 8 | result << 16.
  Raw            // event | umask << 8, on the core PMU.
};

struct EventSpec {
  Counter counter;
  const char *name;     // JSON key.
  const char *encoding; // perf syntax without the PMU and modifiers.
  EventKind kind;
  std::uint64_t config; // Low 32 bits only; the PMU type is added at open().
};

struct EventTable {
  const char *id; // Recorded with every result; tables measure differently.
  std::array<EventSpec, counterCount> events;
};

// Kernel ABI values from linux/perf_event.h, spelled out so the tables compile
// on every platform. They are checked against the header on Linux below.
namespace perfabi {
inline constexpr std::uint64_t hwCPUCycles = 0;
inline constexpr std::uint64_t hwInstructions = 1;
inline constexpr std::uint64_t cacheL1D = 0;
inline constexpr std::uint64_t cacheLL = 2;
inline constexpr std::uint64_t cacheDTLB = 3;
inline constexpr std::uint64_t cacheNode = 6; // Highest cache id.
inline constexpr std::uint64_t opRead = 0;
inline constexpr std::uint64_t opPrefetch = 2; // Highest op id.
inline constexpr std::uint64_t resultMiss = 1;
} // namespace perfabi

constexpr std::uint64_t rawConfig(std::uint8_t event,
                                  std::uint8_t umask) noexcept {
  return std::uint64_t{event} | std::uint64_t{umask} << 8;
}

constexpr std::uint64_t cacheConfig(std::uint64_t cache, std::uint64_t op,
                                    std::uint64_t result) noexcept {
  return cache | op << 8 | result << 16;
}

// Golden Cove P-core encodings. The miss counters count retired loads only,
// except the dTLB walks, which include walks for speculative loads.
inline constexpr EventTable rawGoldenCove{
    "rawGoldenCove",
    {{
        {Counter::Cycles, "cycles", "cycles", EventKind::Hardware,
         perfabi::hwCPUCycles},
        {Counter::Instructions, "instructions", "instructions",
         EventKind::Hardware, perfabi::hwInstructions},
        // MEM_LOAD_RETIRED.L1_MISS
        {Counter::L1DLoadMisses, "l1dLoadMisses", "event=0xd1,umask=0x08",
         EventKind::Raw, rawConfig(0xd1, 0x08)},
        // MEM_LOAD_RETIRED.L3_MISS
        {Counter::LLCLoadMisses, "llcLoadMisses", "event=0xd1,umask=0x20",
         EventKind::Raw, rawConfig(0xd1, 0x20)},
        // DTLB_LOAD_MISSES.WALK_COMPLETED
        {Counter::DTLBLoadWalks, "dtlbLoadWalks", "event=0x12,umask=0x0e",
         EventKind::Raw, rawConfig(0x12, 0x0e)},
    }}};

// The kernel's generic events. On Golden Cove the L1D one counts demand data
// reads reaching L2 (about twice the raw count) and the LLC one an off-core
// response, so results from this table are not comparable with the raw one.
inline constexpr EventTable genericEvents{
    "generic",
    {{
        {Counter::Cycles, "cycles", "cycles", EventKind::Hardware,
         perfabi::hwCPUCycles},
        {Counter::Instructions, "instructions", "instructions",
         EventKind::Hardware, perfabi::hwInstructions},
        {Counter::L1DLoadMisses, "l1dLoadMisses", "L1-dcache-load-misses",
         EventKind::HardwareCache,
         cacheConfig(perfabi::cacheL1D, perfabi::opRead, perfabi::resultMiss)},
        {Counter::LLCLoadMisses, "llcLoadMisses", "LLC-load-misses",
         EventKind::HardwareCache,
         cacheConfig(perfabi::cacheLL, perfabi::opRead, perfabi::resultMiss)},
        {Counter::DTLBLoadWalks, "dtlbLoadWalks", "dTLB-load-misses",
         EventKind::HardwareCache,
         cacheConfig(perfabi::cacheDTLB, perfabi::opRead, perfabi::resultMiss)},
    }}};

namespace detail {

consteval bool validTable(const EventTable &table) {
  for (std::size_t i = 0; i < counterCount; ++i) {
    const EventSpec &spec = table.events[i];
    if (index(spec.counter) != i || !spec.name || !spec.encoding ||
        std::string_view(spec.name).empty() || spec.config >> 32 != 0)
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (std::string_view(spec.name) == table.events[j].name)
        return false;
    if (spec.kind == EventKind::Raw && (spec.config & 0xff) == 0)
      return false;
    if (spec.kind == EventKind::HardwareCache &&
        ((spec.config & 0xff) > perfabi::cacheNode ||
         (spec.config >> 8 & 0xff) > perfabi::opPrefetch ||
         (spec.config >> 16 & 0xff) > perfabi::resultMiss ||
         spec.config >> 24 != 0))
      return false;
  }
  return true;
}

} // namespace detail

static_assert(detail::validTable(rawGoldenCove));
static_assert(detail::validTable(genericEvents));

// A cumulative reading of the group. Only the difference of two is meaningful.
struct CounterSnapshot {
  std::array<std::uint64_t, counterCount> values{};
  std::uint64_t timeEnabled{0}, timeRunning{0};
  bool ok{false};
};

enum class CounterStatus { Ok, Multiplexed, NotScheduled, ReadFailed };

inline const char *toString(CounterStatus status) noexcept {
  switch (status) {
  case CounterStatus::Ok:
    return "ok";
  case CounterStatus::Multiplexed:
    return "multiplexed";
  case CounterStatus::NotScheduled:
    return "notScheduled";
  case CounterStatus::ReadFailed:
    return "readFailed";
  }
  return "unknown";
}

// The counts for one interval, unscaled.
struct CounterResult {
  std::array<std::uint64_t, counterCount> values{};
  std::uint64_t enabledNs{0}, runningNs{0};
  CounterStatus status{CounterStatus::ReadFailed};

  std::uint64_t value(Counter c) const noexcept { return values[index(c)]; }
  double multiplexRatio() const noexcept {
    return enabledNs == 0 ? 0.0
                          : static_cast<double>(runningNs) /
                                static_cast<double>(enabledNs);
  }
};

inline CounterResult diff(const CounterSnapshot &begin,
                          const CounterSnapshot &end) noexcept {
  CounterResult r;
  if (!begin.ok || !end.ok)
    return r; // Zeroed values, so a failed read cannot be mistaken for data.
  for (std::size_t i = 0; i < counterCount; ++i)
    r.values[i] = end.values[i] - begin.values[i];
  r.enabledNs = end.timeEnabled - begin.timeEnabled;
  r.runningNs = end.timeRunning - begin.timeRunning;
  // An unmultiplexed per-thread group advances both times together, so any
  // difference means it lost the counters for part of the interval.
  if (r.runningNs == 0)
    r.status = CounterStatus::NotScheduled;
  else if (r.runningNs != r.enabledNs)
    r.status = CounterStatus::Multiplexed;
  else
    r.status = CounterStatus::Ok;
  return r;
}

// The median counts of an empty region: the floor every region pays for its
// own bookkeeping. Recorded, never subtracted.
struct CounterOverhead {
  std::array<std::uint64_t, counterCount> median{};
  bool ok{false};
};

enum class CounterMode { Off, On, Required };
enum class TableChoice { Auto, Raw, Generic };

inline const char *toString(CounterMode mode) noexcept {
  switch (mode) {
  case CounterMode::Off:
    return "off";
  case CounterMode::On:
    return "on";
  case CounterMode::Required:
    return "required";
  }
  return "unknown";
}

inline std::optional<CounterMode> parseCounterMode(std::string_view text) {
  if (text == "off")
    return CounterMode::Off;
  if (text == "on")
    return CounterMode::On;
  if (text == "required")
    return CounterMode::Required;
  return std::nullopt;
}

inline std::optional<TableChoice> parseTableChoice(std::string_view text) {
  if (text == "auto")
    return TableChoice::Auto;
  if (text == "raw")
    return TableChoice::Raw;
  if (text == "generic")
    return TableChoice::Generic;
  return std::nullopt;
}

namespace detail {

// Groups larger than this are never opened; the selftest's hog group is the
// only one above counterCount.
inline constexpr std::size_t maxGroupEvents = 16;

// Small sysfs and procfs files, trimmed. Setup-time only.
inline std::optional<std::string> readSmallFile(const char *path) {
#if defined(__linux__)
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return std::nullopt;
  char buffer[256];
  ssize_t n = ::read(fd, buffer, sizeof(buffer));
  ::close(fd);
  if (n < 0)
    return std::nullopt;
  std::string text(buffer, static_cast<std::size_t>(n));
  while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
    text.pop_back();
  return text;
#else
  (void)path;
  return std::nullopt;
#endif
}

// The PMU the events are opened on. Hybrid kernels expose the P-core PMU as
// cpu_core and need its type in the high config bits of generic events;
// other kernels reject those bits.
struct PMUInfo {
  std::string device; // sysfs name, empty if none was found
  std::string model;  // caps/pmu_name, e.g. "alderlake_hybrid"
  std::uint32_t type{0};
  bool hybrid{false};
};

inline PMUInfo detectPMU() {
  PMUInfo pmu;
  for (const char *device : {"cpu_core", "cpu"}) {
    std::string base = std::string("/sys/bus/event_source/devices/") + device;
    std::optional<std::string> type = readSmallFile((base + "/type").c_str());
    if (!type)
      continue;
    pmu.device = device;
    pmu.type =
        static_cast<std::uint32_t>(std::strtoul(type->c_str(), nullptr, 10));
    pmu.hybrid = pmu.device == "cpu_core";
    pmu.model = readSmallFile((base + "/caps/pmu_name").c_str()).value_or("");
    break;
  }
  return pmu;
}

// PMUs whose P-core raw encodings have been checked with the selftest.
inline bool rawTableVerified(std::string_view model) noexcept {
  return model == "alderlake_hybrid";
}

#if defined(__linux__)

static_assert(perfabi::hwCPUCycles == PERF_COUNT_HW_CPU_CYCLES);
static_assert(perfabi::hwInstructions == PERF_COUNT_HW_INSTRUCTIONS);
static_assert(perfabi::cacheL1D == PERF_COUNT_HW_CACHE_L1D);
static_assert(perfabi::cacheLL == PERF_COUNT_HW_CACHE_LL);
static_assert(perfabi::cacheDTLB == PERF_COUNT_HW_CACHE_DTLB);
static_assert(perfabi::cacheNode == PERF_COUNT_HW_CACHE_NODE);
static_assert(perfabi::opRead == PERF_COUNT_HW_CACHE_OP_READ);
static_assert(perfabi::opPrefetch == PERF_COUNT_HW_CACHE_OP_PREFETCH);
static_assert(perfabi::resultMiss == PERF_COUNT_HW_CACHE_RESULT_MISS);

inline perf_event_attr makeAttr(const EventSpec &spec, const PMUInfo &pmu,
                                bool leader) noexcept {
  perf_event_attr attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  std::uint64_t extendedType =
      pmu.hybrid ? std::uint64_t{pmu.type} << PERF_PMU_TYPE_SHIFT : 0;
  switch (spec.kind) {
  case EventKind::Hardware:
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = spec.config | extendedType;
    break;
  case EventKind::HardwareCache:
    attr.type = PERF_TYPE_HW_CACHE;
    attr.config = spec.config | extendedType;
    break;
  case EventKind::Raw:
    attr.type = pmu.type;
    attr.config = spec.config;
    break;
  }
  // User mode only: perf_event_paranoid=2 allows nothing else unprivileged.
  // Keep in sync with the "u" modifier in encodingOf().
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID |
                     PERF_FORMAT_TOTAL_TIME_ENABLED |
                     PERF_FORMAT_TOTAL_TIME_RUNNING;
  if (leader) {
    attr.disabled = 1;
    // A pinned group never multiplexes: if it loses its counters it enters an
    // error state and reads return nothing, which shows up as ReadFailed.
    attr.pinned = 1;
  }
  return attr;
}

inline std::string describeOpenError(int error, const EventSpec &spec,
                                     const PMUInfo &pmu) {
  std::string_view device = pmu.device.empty() ? std::string_view("default")
                                               : std::string_view(pmu.device);
  switch (error) {
  case EACCES:
  case EPERM:
    return std::format(
        "perf_event_open denied (perf_event_paranoid={}); user-mode counting "
        "needs paranoid <= 2 or CAP_PERFMON",
        readSmallFile("/proc/sys/kernel/perf_event_paranoid").value_or("?"));
  case ENOENT:
  case EOPNOTSUPP:
    return std::format("event {} ({}) is not supported by PMU {}", spec.name,
                       spec.encoding, device);
  case EINVAL:
    return std::format("event {} ({}) was rejected by PMU {}: unsupported, or "
                       "the group cannot fit in its counters",
                       spec.name, spec.encoding, device);
  case ENODEV:
    return std::format("PMU {} cannot count on this CPU; on hybrid CPUs pin "
                       "to a P-core",
                       device);
  case EBUSY:
    return "the PMU is held exclusively by another user";
  case EMFILE:
  case ENFILE:
    return "out of file descriptors opening perf events";
  default:
    return std::format("perf_event_open failed for {}: {}", spec.name,
                       std::strerror(error));
  }
}

inline void closeAll(std::span<int> fds) noexcept {
  // Members first, then the leader, though the order does not matter.
  for (std::size_t i = fds.size(); i-- > 0;) {
    if (fds[i] >= 0)
      ::close(fds[i]);
    fds[i] = -1;
  }
}

// Opens specs as one pinned group (specs[0] leads), fills fds and ids, and
// enables it. All or nothing: on failure every opened fd is closed.
inline std::expected<void, std::string>
openGroup(std::span<const EventSpec> specs, const PMUInfo &pmu,
          std::span<int> fds, std::span<std::uint64_t> ids) {
  if (specs.empty() || specs.size() > maxGroupEvents ||
      fds.size() != specs.size() || ids.size() != specs.size())
    return std::unexpected(std::string("invalid perf group size"));
  for (std::size_t i = 0; i < specs.size(); ++i) {
    perf_event_attr attr = makeAttr(specs[i], pmu, i == 0);
    int groupFd = i == 0 ? -1 : fds[0];
    long fd = ::syscall(SYS_perf_event_open, &attr, 0, -1, groupFd,
                        PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) {
      int error = errno;
      closeAll(fds.first(i));
      return std::unexpected(describeOpenError(error, specs[i], pmu));
    }
    fds[i] = static_cast<int>(fd);
    if (::ioctl(fds[i], PERF_EVENT_IOC_ID, &ids[i]) != 0) {
      int error = errno;
      closeAll(fds.first(i + 1));
      return std::unexpected(std::format("PERF_EVENT_IOC_ID failed for {}: {}",
                                         specs[i].name, std::strerror(error)));
    }
  }
  if (::ioctl(fds[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0 ||
      ::ioctl(fds[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
    int error = errno;
    closeAll(fds);
    return std::unexpected(std::format("enabling the perf group failed: {}",
                                       std::strerror(error)));
  }
  return {};
}

// The kernel's layout for PERF_FORMAT_GROUP | ID | TOTAL_TIME_ENABLED |
// TOTAL_TIME_RUNNING. Changing read_format in makeAttr() changes this.
struct GroupRead {
  std::uint64_t nr, timeEnabled, timeRunning;
  struct Entry {
    std::uint64_t value, id;
  } entries[maxGroupEvents];
};
static_assert(sizeof(GroupRead) == (3 + 2 * maxGroupEvents) * 8);

// Reads the group and places each value in the slot whose id matches. False
// on a short or empty read, which is what a pinned group in error state gives.
inline bool readGroup(int leader, std::span<const std::uint64_t> ids,
                      std::span<std::uint64_t> values, std::uint64_t &enabled,
                      std::uint64_t &running) noexcept {
  GroupRead buffer;
  std::size_t expected = (3 + 2 * ids.size()) * sizeof(std::uint64_t);
  ssize_t n = ::read(leader, &buffer, sizeof(buffer));
  if (n < 0 || static_cast<std::size_t>(n) != expected ||
      buffer.nr != ids.size())
    return false;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    bool found = false;
    for (std::size_t j = 0; j < ids.size() && !found; ++j) {
      if (buffer.entries[j].id == ids[i]) {
        values[i] = buffer.entries[j].value;
        found = true;
      }
    }
    if (!found)
      return false;
  }
  enabled = buffer.timeEnabled;
  running = buffer.timeRunning;
  return true;
}

#endif // __linux__

} // namespace detail

// The process's counter group. Open it on the thread that runs the measured
// work: the events count the opening thread only.
class PerfGroup {
public:
  static std::expected<PerfGroup, std::string>
  open(TableChoice choice = TableChoice::Auto) {
#if defined(__linux__)
    PerfGroup group;
    group.pmu = detail::detectPMU();
    bool raw = choice == TableChoice::Raw ||
               (choice == TableChoice::Auto &&
                detail::rawTableVerified(group.pmu.model));
    if (raw && group.pmu.device.empty())
      return std::unexpected(
          std::string("the raw event table needs a core PMU in sysfs"));
    group.events = raw ? &rawGoldenCove : &genericEvents;
    auto opened = detail::openGroup(group.events->events, group.pmu, group.fds,
                                    group.ids);
    if (!opened)
      return std::unexpected(std::move(opened.error()));
    return group;
#else
    (void)choice;
    return std::unexpected(std::string("perf counters are Linux only"));
#endif
  }

  PerfGroup(const PerfGroup &) = delete;
  PerfGroup &operator=(const PerfGroup &) = delete;

  PerfGroup(PerfGroup &&other) noexcept
      : fds(std::exchange(other.fds, closedFds())), ids(other.ids),
        events(other.events), pmu(std::move(other.pmu)) {}

  PerfGroup &operator=(PerfGroup &&other) noexcept {
    if (this != &other) {
      close();
      fds = std::exchange(other.fds, closedFds());
      ids = other.ids;
      events = other.events;
      pmu = std::move(other.pmu);
    }
    return *this;
  }

  ~PerfGroup() { close(); }

  // One read() syscall. Out of line so the code around a timed loop is laid
  // out the same whether counters are on or off.
  [[gnu::noinline]] CounterSnapshot snapshot() const noexcept {
    CounterSnapshot s;
#if defined(__linux__)
    s.ok =
        detail::readGroup(fds[0], ids, s.values, s.timeEnabled, s.timeRunning);
#endif
    return s;
  }

  const EventTable &table() const noexcept { return *events; }
  const detail::PMUInfo &pmuInfo() const noexcept { return pmu; }

  // The event as `perf stat -e` accepts it, to reproduce a count by hand.
  std::string encodingOf(const EventSpec &spec) const {
    if (pmu.hybrid || spec.kind == EventKind::Raw)
      return std::format("{}/{}/u", pmu.device, spec.encoding);
    return std::format("{}:u", spec.encoding);
  }

private:
  PerfGroup() = default;

  static std::array<int, counterCount> closedFds() noexcept {
    std::array<int, counterCount> closed;
    closed.fill(-1);
    return closed;
  }

  void close() noexcept {
#if defined(__linux__)
    detail::closeAll(fds);
#endif
  }

  std::array<int, counterCount> fds = closedFds();
  std::array<std::uint64_t, counterCount> ids{};
  const EventTable *events = &genericEvents;
  detail::PMUInfo pmu;
};

} // namespace tabench
