#pragma once

// Process memory footprint for bench results. Readings never allocate, so
// taking one does not disturb the allocator under test. readMemoryStats()
// walks every mapping under mmap_lock and is for outside timed regions only;
// sampleRSS() reads the kernel's RSS counters and is cheap enough to take
// between workload phases.
//
// The headline figure is heldAnonKiB(): resident anonymous memory minus pages
// already returned with MADV_FREE, which stay in Rss until the kernel reclaims
// them. Linux only; elsewhere readings report ok == false.

#include "clock.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tabench {

// One full reading, in KiB as the kernel reports it.
struct MemoryStats {
  // From /proc/self/smaps_rollup.
  std::uint64_t rssKiB{0};
  std::uint64_t anonymousKiB{0};
  std::uint64_t anonHugePagesKiB{0};
  std::uint64_t lazyFreeKiB{0};
  // From /proc/self/status. After resetPeakRSS(), vmHWMKiB is the peak since
  // the reset.
  std::uint64_t vmRSSKiB{0};
  std::uint64_t vmHWMKiB{0};
  bool ok{false};

  std::uint64_t heldAnonKiB() const noexcept {
    return anonymousKiB > lazyFreeKiB ? anonymousKiB - lazyFreeKiB : 0;
  }
};

// A cheap reading from /proc/self/statm.
struct RSSSample {
  std::uint64_t ticks{0};
  std::uint64_t rssBytes{0};
  std::uint64_t anonBytes{0}; // Resident minus file-backed and shmem.
  // Live requested bytes at the same moment, passed in by the workload from
  // the seam's stats, so RSS and live bytes can be plotted on one time axis.
  // The harness doesn't depend on the seam, so it can't read them itself.
  std::uint64_t liveBytes{0};
  bool ok{false};
};

// Everything the record's "memory" field holds. The summary computes heap RSS
// as after.heldAnonKiB() minus the startup baseline and inputBytes.
struct MemoryReport {
  MemoryStats startup; // First thing in main, before any inputs exist.
  MemoryStats before;  // Just before the timed region.
  MemoryStats after;   // Just after it.
  bool peakReset{false};
  std::uint64_t inputBytes{0}; // Input tapes mapped outside the allocator.
  std::span<const RSSSample> samples;
};

namespace detail {

// Reads a whole procfs file into buffer. Returns the byte count, or 0 on error
// or if the file did not fit, so a truncated file is never parsed.
inline std::size_t readProcFile(const char *path, char *buffer,
                                std::size_t size) noexcept {
#if defined(__linux__)
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  std::size_t total = 0;
  while (total < size) {
    ssize_t n = ::read(fd, buffer + total, size - total);
    if (n < 0) {
      ::close(fd);
      return 0;
    }
    if (n == 0)
      break;
    total += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return total < size ? total : 0;
#else
  (void)path;
  (void)buffer;
  (void)size;
  return 0;
#endif
}

// Finds a "Key:   value kB" line and parses its value. The key must match a
// whole field name, so "Rss" does not match "RssAnon".
inline bool findField(std::string_view text, std::string_view key,
                      std::uint64_t &value) noexcept {
  std::size_t lineStart = 0;
  while (lineStart < text.size()) {
    std::size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string_view::npos)
      lineEnd = text.size();
    std::string_view line = text.substr(lineStart, lineEnd - lineStart);
    if (line.size() > key.size() && line.starts_with(key) &&
        line[key.size()] == ':') {
      std::size_t pos = key.size() + 1;
      while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;
      auto [ptr, ec] =
          std::from_chars(line.data() + pos, line.data() + line.size(), value);
      return ec == std::errc{} && ptr != line.data() + pos;
    }
    lineStart = lineEnd + 1;
  }
  return false;
}

inline std::uint64_t pageBytes() noexcept {
#if defined(__linux__)
  static const std::uint64_t bytes =
      static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
  return bytes;
#else
  return 4096;
#endif
}

} // namespace detail

inline MemoryStats readMemoryStats() noexcept {
  MemoryStats s;
  // Both files are under 2 KiB on current kernels; readProcFile() fails rather
  // than truncate if one ever outgrows this.
  char buffer[8192];
  std::size_t n =
      detail::readProcFile("/proc/self/smaps_rollup", buffer, sizeof(buffer));
  if (n == 0)
    return s;
  std::string_view rollup(buffer, n);
  bool ok = detail::findField(rollup, "Rss", s.rssKiB) &&
            detail::findField(rollup, "Anonymous", s.anonymousKiB) &&
            detail::findField(rollup, "AnonHugePages", s.anonHugePagesKiB) &&
            detail::findField(rollup, "LazyFree", s.lazyFreeKiB);
  n = detail::readProcFile("/proc/self/status", buffer, sizeof(buffer));
  if (n == 0)
    return s;
  std::string_view status(buffer, n);
  ok = ok && detail::findField(status, "VmRSS", s.vmRSSKiB) &&
       detail::findField(status, "VmHWM", s.vmHWMKiB);
  s.ok = ok;
  return s;
}

// Resets VmHWM to the current RSS, so a later reading's vmHWMKiB is the peak
// since now. False if the write was refused.
inline bool resetPeakRSS() noexcept {
#if defined(__linux__)
  int fd = ::open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  bool ok = ::write(fd, "5", 1) == 1;
  ::close(fd);
  return ok;
#else
  return false;
#endif
}

inline RSSSample sampleRSS(std::uint64_t liveBytes = 0) noexcept {
  RSSSample s;
  s.ticks = readTicks();
  s.liveBytes = liveBytes;
  // Fields, in pages: size resident shared text lib data dt.
  char buffer[256];
  std::size_t n =
      detail::readProcFile("/proc/self/statm", buffer, sizeof(buffer));
  if (n == 0)
    return s;
  const char *p = buffer;
  const char *end = buffer + n;
  std::uint64_t fields[3];
  for (std::uint64_t &field : fields) {
    while (p < end && *p == ' ')
      ++p;
    auto [ptr, ec] = std::from_chars(p, end, field);
    if (ec != std::errc{})
      return s;
    p = ptr;
  }
  std::uint64_t resident = fields[1], shared = fields[2];
  s.rssBytes = resident * detail::pageBytes();
  s.anonBytes =
      (resident > shared ? resident - shared : 0) * detail::pageBytes();
  s.ok = true;
  return s;
}

} // namespace tabench
