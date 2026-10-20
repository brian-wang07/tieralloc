#pragma once

// The machine state the method requires with every result: CPU model, kernel,
// THP mode and frequency-scaling state. Read outside timed regions; a field
// that cannot be read (containers, other OSes) is empty, never an error.

#include "perf.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace tabench {

struct SystemInfo {
  std::optional<std::string> cpuModel;
  std::optional<std::string> kernel;
  std::optional<std::string> thp;       // the selected mode, e.g. "always"
  std::optional<std::string> thpDefrag; // likewise
  std::optional<std::string> governor;  // of the CPU the region ran on
  int cpu{-1};
};

namespace detail {

// The bracketed choice in a sysfs mode file: "always [madvise] never" gives
// "madvise".
inline std::optional<std::string> selectedMode(const char *path) {
  std::optional<std::string> text = readSmallFile(path);
  if (!text)
    return std::nullopt;
  const std::size_t open = text->find('[');
  const std::size_t close = text->find(']', open);
  if (open == std::string::npos || close == std::string::npos)
    return std::nullopt;
  return text->substr(open + 1, close - open - 1);
}

// /proc/cpuinfo is larger than readSmallFile's buffer, so it is streamed.
inline std::optional<std::string> cpuModelName() {
  std::ifstream in("/proc/cpuinfo");
  std::string line;
  constexpr std::string_view key = "model name";
  while (std::getline(in, line)) {
    if (!line.starts_with(key))
      continue;
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos)
      return std::nullopt;
    const std::size_t start = line.find_first_not_of(' ', colon + 1);
    return start == std::string::npos ? std::string() : line.substr(start);
  }
  return std::nullopt;
}

} // namespace detail

// All interrupts delivered to cpu so far (every row of /proc/interrupts, timer
// included), or empty if unknown. Read just outside a region, so it must not
// allocate: the file goes into a static buffer, parsed in place.
inline std::optional<std::uint64_t> interruptsOn(int cpu) noexcept {
#if defined(__linux__)
  if (cpu < 0)
    return std::nullopt;
  static char buffer[1 << 18];
  const int fd = ::open("/proc/interrupts", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return std::nullopt;
  std::size_t size = 0;
  for (ssize_t n; size < sizeof(buffer) &&
                  (n = ::read(fd, buffer + size, sizeof(buffer) - size)) > 0;)
    size += static_cast<std::size_t>(n);
  ::close(fd);
  if (size == 0 || size == sizeof(buffer))
    return std::nullopt; // empty, or too large to trust
  const std::string_view text(buffer, size);

  // The header names the CPU columns; find cpu's position among them.
  const std::size_t headerEnd = text.find('\n');
  if (headerEnd == std::string_view::npos)
    return std::nullopt;
  const std::string_view header = text.substr(0, headerEnd);
  int column = -1;
  int seen = 0;
  for (std::size_t at = header.find("CPU"); at != std::string_view::npos;
       at = header.find("CPU", at + 3), ++seen) {
    std::size_t end = at + 3;
    while (end < header.size() && header[end] >= '0' && header[end] <= '9')
      ++end;
    int number = 0;
    std::from_chars(header.data() + at + 3, header.data() + end, number);
    if (number == cpu) {
      column = seen;
      break;
    }
  }
  if (column < 0)
    return std::nullopt;

  // Each row: "label:" then one count per CPU column (some rows have fewer).
  std::uint64_t total = 0;
  for (std::size_t line = headerEnd + 1; line < text.size();) {
    std::size_t lineEnd = text.find('\n', line);
    if (lineEnd == std::string_view::npos)
      lineEnd = text.size();
    const std::string_view row = text.substr(line, lineEnd - line);
    line = lineEnd + 1;
    const std::size_t colon = row.find(':');
    if (colon == std::string_view::npos)
      continue;
    const char *p = row.data() + colon + 1;
    const char *end = row.data() + row.size();
    for (int c = 0; c <= column; ++c) {
      while (p < end && *p == ' ')
        ++p;
      std::uint64_t value = 0;
      auto [next, ec] = std::from_chars(p, end, value);
      if (ec != std::errc{})
        break;
      if (c == column)
        total += value;
      p = next;
    }
  }
  return total;
#else
  (void)cpu;
  return std::nullopt;
#endif
}

// This thread's context switches so far, from getrusage.
struct ContextSwitches {
  std::uint64_t voluntary{0};
  std::uint64_t involuntary{0};
  bool ok{false};
};

inline ContextSwitches contextSwitches() noexcept {
  ContextSwitches c;
#if defined(__linux__)
  rusage usage{};
  if (::getrusage(RUSAGE_THREAD, &usage) == 0)
    c = {static_cast<std::uint64_t>(usage.ru_nvcsw),
         static_cast<std::uint64_t>(usage.ru_nivcsw), true};
#endif
  return c;
}

// cpu is the CPU whose governor to report, or -1 for none.
inline SystemInfo readSystemInfo(int cpu) {
  SystemInfo info;
  info.cpu = cpu;
#if defined(__linux__)
  info.cpuModel = detail::cpuModelName();
  if (utsname name; ::uname(&name) == 0)
    info.kernel = name.release;
  info.thp =
      detail::selectedMode("/sys/kernel/mm/transparent_hugepage/enabled");
  info.thpDefrag =
      detail::selectedMode("/sys/kernel/mm/transparent_hugepage/defrag");
  if (cpu >= 0) {
    const std::string path = "/sys/devices/system/cpu/cpu" +
                             std::to_string(cpu) + "/cpufreq/scaling_governor";
    info.governor = detail::readSmallFile(path.c_str());
  }
#endif
  return info;
}

} // namespace tabench
