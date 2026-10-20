#pragma once

// The machine state the method requires with every result: CPU model, kernel,
// THP mode and frequency-scaling state. Read outside timed regions; a field
// that cannot be read (containers, other OSes) is empty, never an error.

#include "perf.hpp"

#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#if defined(__linux__)
#include <sys/utsname.h>
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
