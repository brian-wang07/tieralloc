#pragma once

// main watcher loop. while the program runs, the loop updates the heartbeat
// every ~1 millisecond, catches exits, collects the status and marks the pool
// as stopping.

#include "launcher/signals.hpp"
#include "launcher/spawn.hpp"
#include "tatrace/pool.hpp"

#include <array>
#include <cerrno>
#include <expected>

#include <poll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace tatrace::launcher {

inline constexpr int watchIntervalMs = 1;

struct WatchError {
  const char *step; // "pidfd_open", "poll"
  int error;
};

namespace detail {

inline int openPidFd(pid_t pid) noexcept {
  // glibc provides no wrapper for pidfd_open(), necessitating the use of
  // syscall().
  long fd = ::syscall(SYS_pidfd_open, pid, 0);
  return static_cast<int>(fd);
}
} // namespace detail

inline std::expected<int, WatchError> watchTarget(const Pool &pool, pid_t pid,
                                                  int signalFd) {
  int pidfd = detail::openPidFd(pid);
  if (pidfd == -1)
    return std::unexpected(WatchError{"pidfd_open", errno});

  std::array<pollfd, 2> fds{{
      {pidfd, POLLIN, 0},
      {signalFd, POLLIN, 0}, // -1 when theres no forwarding
  }};

  for (;;) {
    // first, beat on every pass
    tatrace::beat(pool);

    int n = ::poll(fds.data(), fds.size(), watchIntervalMs);
    if (n == -1) {
      if (errno == EINTR)
        continue;
      int error = errno; // close may overwrite
      ::close(pidfd);
      return std::unexpected(WatchError{"poll", error});
    }

    if (fds[1].revents & POLLIN)
      forwardPending(signalFd, pid);

    if (fds[0].revents & POLLIN)
      break;
  }

  // pid is always a zombie here so this doesnt block
  int status = waitFor(pid);

  // no more drains
  tatrace::stop(pool);
  ::close(pidfd);
  return status;
}
} // namespace tatrace::launcher
