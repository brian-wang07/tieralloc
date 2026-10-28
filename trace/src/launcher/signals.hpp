#pragma once

// while the program runs, the launcher must survive the signals meant for the
// program, pass them to the program once, then exit with the program's status.

#include <array>
#include <cerrno>
#include <expected>
#include <utility>

#include <signal.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <unistd.h>

namespace tatrace::launcher {

inline constexpr std::array<int, 4> forwardedSignals{SIGINT, SIGQUIT, SIGTERM,
                                                     SIGHUP};

struct SignalError {
  const char *step; // "sigprocmask", "signalfd"
  int error;
};

// blocks forwardedSignals and reads them from a signalfd. the destructor drains
// the signalfd, restores the previous mask and closes it. is movable but not
// copyable
class ForwardedSignals {
public:
  static std::expected<ForwardedSignals, SignalError> create() {
    sigset_t set;
    ::sigemptyset(&set);

    for (const auto sig : forwardedSignals)
      ::sigaddset(&set, sig);

    sigset_t previous;
    if (::sigprocmask(SIG_BLOCK, &set, &previous) == -1)
      return std::unexpected(SignalError{"sigprocmask", errno});

    int fd = ::signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd == -1) {
      int error = errno;
      ::sigprocmask(SIG_SETMASK, &previous, nullptr);
      return std::unexpected(SignalError{"signalfd", error});
    }
    return ForwardedSignals(fd, previous);
  }
  ForwardedSignals(ForwardedSignals &&other) noexcept
      : previous(other.previous), signalFd(std::exchange(other.signalFd, -1)) {}
  ForwardedSignals &operator=(ForwardedSignals &&) = delete;
  ~ForwardedSignals() {
    // moved from: noop
    if (signalFd < 0)
      return;

    // a signal still pending when the mask comes back would be delivered with
    // its default action and kill the launcher, so drain first
    signalfd_siginfo info;
    for (;;) {
      ssize_t n = ::read(signalFd, &info, sizeof(info));
      if (n == -1 && errno == EINTR)
        continue;
      if (n != static_cast<ssize_t>(sizeof(info)))
        break; // -1 with EAGAIN
    }

    // put the mask back as it was before create
    ::sigprocmask(SIG_SETMASK, &previous, nullptr);
    ::close(signalFd);
  }
  int fd() const noexcept { return signalFd; }

private:
  ForwardedSignals(int fd, const sigset_t &previous)
      : previous(previous), signalFd(fd) {}
  sigset_t previous{};
  int signalFd = -1;
};

// forward unless the kernel sent it
inline bool shouldForward(const signalfd_siginfo &info) noexcept {
  return info.ssi_code != SI_KERNEL;
}

// reads every pending record and forwards the ones that should be
inline void forwardPending(int signalFd, pid_t pid) noexcept {
  signalfd_siginfo info;
  for (;;) {
    ssize_t n = ::read(signalFd, &info, sizeof(info));
    if (n == -1 && errno == EINTR)
      continue;
    if (n != static_cast<ssize_t>(sizeof(info)))
      return; // -1 with EAGAIN, same as ForwardedSignals' dtor
    if (shouldForward(info))
      ::kill(pid, static_cast<int>(info.ssi_signo));
  }
}

} // namespace tatrace::launcher
