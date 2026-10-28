// Checks for the launcher's signal forwarding (src/launcher/signals.hpp).
// shouldForward is checked on hand-built records, since a terminal's
// SI_KERNEL signal can't be produced without a pty. ForwardedSignals is
// checked for what it blocks, its fd's flags, a raised signal arriving on the
// fd instead of killing us, the mask coming back and the fd closing on
// destruction with a signal still pending, and a moved-from object doing
// nothing. forwardPending is checked against a real child, then the whole
// path through watchTarget, then the real launcher receiving SIGTERM and
// SIGINT. Exits nonzero if any check fails.

#include "launcher/signals.hpp"
#include "launcher/spawn.hpp"
#include "launcher/watch.hpp"
#include "ta/testing.hpp"
#include "tatrace/pool.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <expected>
#include <optional>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace tatrace;
using namespace tatrace::launcher;
using namespace ta::test;

sigset_t currentMask() {
  sigset_t mask;
  ::sigprocmask(SIG_BLOCK, nullptr, &mask);
  return mask;
}

bool allBlocked(const sigset_t &mask) {
  for (int sig : forwardedSignals)
    if (::sigismember(&mask, sig) != 1)
      return false;
  return true;
}

bool sameMask(const sigset_t &a, const sigset_t &b) {
  for (int sig = 1; sig < NSIG; ++sig)
    if (::sigismember(&a, sig) != ::sigismember(&b, sig))
      return false;
  return true;
}

bool readable(int fd) {
  pollfd p{fd, POLLIN, 0};
  return ::poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

signalfd_siginfo record(int code, int sig) {
  signalfd_siginfo info{};
  info.ssi_code = code;
  info.ssi_signo = static_cast<std::uint32_t>(sig);
  return info;
}

void checkShouldForward() {
  check(shouldForward(record(SI_USER, SIGTERM)), "rule",
        "kill(SIGTERM) aimed at the launcher is forwarded");
  check(shouldForward(record(SI_USER, SIGINT)), "rule",
        "kill(SIGINT) is forwarded");
  check(shouldForward(record(SI_QUEUE, SIGHUP)), "rule",
        "sigqueue(SIGHUP) is forwarded");
  check(!shouldForward(record(SI_KERNEL, SIGINT)), "rule",
        "a terminal's SIGINT isn't: the program's group already got it");
  check(!shouldForward(record(SI_KERNEL, SIGHUP)), "rule",
        "a terminal hangup isn't either");
}

void checkLifetime() {
  sigset_t before = currentMask();
  int fd = -1;
  {
    auto signals = ForwardedSignals::create();
    if (!signals) {
      check(false, "life", "create failed at %s: %s", signals.error().step,
            std::strerror(signals.error().error));
      return;
    }
    fd = signals->fd();
    check(allBlocked(currentMask()), "life",
          "create blocks SIGINT, SIGQUIT, SIGTERM and SIGHUP");
    check(::fcntl(fd, F_GETFD) == FD_CLOEXEC &&
              (::fcntl(fd, F_GETFL) & O_NONBLOCK),
          "life", "the signalfd is close-on-exec and nonblocking");
    check(!readable(fd), "life", "nothing is pending at first");
    ::raise(SIGHUP);
    check(readable(fd), "life",
          "a raised SIGHUP arrives on the fd instead of killing us");
    // left pending on purpose: the destructor has to drain it
  }
  check(sameMask(currentMask(), before), "life",
        "after destruction, the mask is what it was before create");
  check(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "life",
        "the fd is closed");
  check(true, "life",
        "the pending SIGHUP was drained, not delivered: we're still alive");
}

void checkMove() {
  sigset_t before = currentMask();
  auto signals = ForwardedSignals::create();
  if (!signals) {
    check(false, "move", "create failed");
    return;
  }
  int fd = signals->fd();
  std::optional<ForwardedSignals> moved(std::move(*signals));
  // destroys the moved-from object now
  signals = std::unexpected(SignalError{"reset", 0});
  check(allBlocked(currentMask()) && ::fcntl(fd, F_GETFD) != -1, "move",
        "destroying a moved-from object leaves the block and the fd alone");
  check(moved->fd() == fd, "move", "the moved-to object owns the fd");
  moved.reset();
  check(sameMask(currentMask(), before) && ::fcntl(fd, F_GETFD) == -1, "move",
        "destroying the moved-to object restores the mask and closes the fd");
}

// In a forked child of a process that blocked forwardedSignals: back to the
// defaults, as becomeTarget does for the real program.
void unblockForwarded() {
  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  for (int sig : forwardedSignals)
    ::sigaction(sig, &dfl, nullptr);
  sigset_t empty;
  ::sigemptyset(&empty);
  ::sigprocmask(SIG_SETMASK, &empty, nullptr);
}

template <class F> pid_t forkChild(F f) {
  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid == 0) {
    unblockForwarded();
    ::_exit(f());
  }
  return pid;
}

void checkForwardPending() {
  auto signals = ForwardedSignals::create();
  if (!signals) {
    check(false, "forward", "create failed");
    return;
  }
  pid_t child = forkChild([] {
    ::pause();
    return 0;
  });
  ::raise(SIGTERM);
  forwardPending(signals->fd(), child);
  int status = waitFor(child);
  check(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "forward",
        "a SIGTERM raised in the launcher reaches the child");
  check(!readable(signals->fd()), "forward",
        "forwardPending reads everything that was pending");
}

void checkThroughLoop(int sig, int wantCode) {
  auto pool = createPool({2, 1024, defaultHeartbeatTimeoutNs});
  if (!pool) {
    check(false, "loop", "createPool failed");
    return;
  }
  {
    // before the fork, as main orders it
    auto signals = ForwardedSignals::create();
    if (!signals) {
      check(false, "loop", "create failed");
    } else {
      auto start = std::chrono::steady_clock::now();
      pid_t child = forkChild([sig] {
        ::kill(::getppid(), sig);
        ::sleep(5);
        return 0;
      });
      auto status = watchTarget(pool->pool, child, signals->fd());
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count();
      check(status && exitCodeFor(*status) == wantCode && ms < 2000, "loop",
            "%s sent to the watcher is forwarded: the child dies with code %d "
            "after %lld ms, not 5 s",
            sigabbrev_np(sig), wantCode, static_cast<long long>(ms));
    }
  }
  destroyPool(*pool);
}

// The real launcher on sh -c 'sleep 5', sent sig after 300 ms. Its exit code,
// or -1 if it couldn't be run.
int launcherCodeAfter(int sig, long long &ms) {
  std::fflush(nullptr);
  auto start = std::chrono::steady_clock::now();
  pid_t launcher = ::fork();
  if (launcher == 0) {
    // the "killed by" line isn't checked here; keep the output readable
    int devNull = ::open("/dev/null", O_WRONLY);
    ::dup2(devNull, STDERR_FILENO);
    ::execl(TA_LAUNCHER, TA_LAUNCHER, "run", "--", "/bin/sh", "-c", "sleep 5",
            static_cast<char *>(nullptr));
    ::_exit(127);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  ::kill(launcher, sig);
  int status = waitFor(launcher);
  ms = std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now() - start)
           .count();
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void checkLauncher(int sig, int wantCode) {
  long long ms = 0;
  int code = launcherCodeAfter(sig, ms);
  check(code == wantCode && ms < 2000, "launcher",
        "kill -%s to tieralloc-trace stops the program; the launcher exits "
        "%d after %lld ms (wanted %d)",
        sigabbrev_np(sig), code, ms, wantCode);
}

} // namespace

int main() {
  checkShouldForward();
  checkLifetime();
  checkMove();
  checkForwardPending();
  checkThroughLoop(SIGTERM, 128 + SIGTERM);
  checkThroughLoop(SIGINT, 128 + SIGINT);
  checkLauncher(SIGTERM, 128 + SIGTERM);
  checkLauncher(SIGINT, 128 + SIGINT);
  return finish();
}
