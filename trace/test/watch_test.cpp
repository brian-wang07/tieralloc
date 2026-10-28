// Checks for the watcher loop (src/launcher/watch.hpp). Forked stand-ins play
// the program: they inherit the pool mapping, so they read the heartbeat
// directly. Checks that the heartbeat stays fresh for several timeouts, that
// an exit is noticed within milliseconds, that the raw status comes back for
// exits and signal deaths, that stopping is set and the pidfd closed, that
// EINTR from a stream of signals is retried without a stale beat, that a pid
// with no process is a pidfd_open error, and one end-to-end run of a real
// program with the shim attached. Exits nonzero if any check fails.

#include "launcher/spawn.hpp"
#include "launcher/watch.hpp"
#include "ta/testing.hpp"
#include "ta/ticks.hpp"
#include "tatrace/pool.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace tatrace;
using namespace tatrace::launcher;
using namespace ta::test;

// Short, so "fresh" is measured against something tight.
constexpr std::uint64_t testTimeoutNs = 200'000'000;
// A heartbeat older than this counts as stale. 50x the interval, so a loaded
// machine doesn't make the check flaky.
constexpr std::int64_t staleNs = 50'000'000;

// A pool destroyed on every path out of a check.
class LivePool {
public:
  LivePool() : created(createPool({2, 1024, testTimeoutNs})) {
    if (!created)
      check(false, "create", "createPool failed at %s: %s",
            created.error().step, std::strerror(created.error().error));
  }
  LivePool(const LivePool &) = delete;
  LivePool &operator=(const LivePool &) = delete;
  ~LivePool() {
    if (created)
      destroyPool(*created);
  }

  explicit operator bool() const { return created.has_value(); }
  const Pool &pool() const { return created->pool; }
  int fd() const { return created->fd; }

private:
  std::expected<CreatedPool, PoolError> created;
};

// Forks a child that runs f and exits with its result.
template <class F> pid_t forkChild(F f) {
  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid == 0)
    ::_exit(f());
  return pid;
}

// In a child: samples the inherited heartbeat every 5 ms for durationMs.
// 0 if every sample was fresh, 1 if any was stale.
int heartbeatStaysFresh(const Pool &pool, int durationMs) {
  const auto &heartbeat = pool.header->watcher.liveness.heartbeatNs;
  std::int64_t end = ta::monotonicNs() + std::int64_t{durationMs} * 1'000'000;
  while (ta::monotonicNs() < end) {
    // loaded before now is read, so the age can't come out negative
    auto beat =
        static_cast<std::int64_t>(heartbeat.load(std::memory_order_relaxed));
    if (ta::monotonicNs() - beat > staleNs)
      return 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return 0;
}

std::vector<int> openFds() {
  std::vector<int> fds;
  for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd"))
    fds.push_back(std::atoi(entry.path().filename().c_str()));
  // the iterator's own directory fd is in the listing; it's closed now
  std::erase_if(fds, [](int fd) { return ::fcntl(fd, F_GETFD) == -1; });
  return fds;
}

long long msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

void checkHeartbeat() {
  LivePool live;
  if (!live)
    return;
  // 5x the timeout: a loop that stopped beating would be caught many times
  // over.
  pid_t pid = forkChild([&] { return heartbeatStaysFresh(live.pool(), 1000); });
  auto status = watchTarget(live.pool(), pid);
  check(status && exitCodeFor(*status) == 0, "beat",
        "the heartbeat stays under %lld ms old for 1 s, 5x the timeout",
        static_cast<long long>(staleNs / 1'000'000));
}

void checkPromptExit() {
  LivePool live;
  if (!live)
    return;
  auto start = std::chrono::steady_clock::now();
  pid_t pid = forkChild([] { return 3; });
  auto status = watchTarget(live.pool(), pid);
  long long ms = msSince(start);
  check(status && exitCodeFor(*status) == 3, "exit",
        "an exit(3) comes back as status 3");
  check(status && ms < 100, "exit",
        "watchTarget returns %lld ms after the fork, well under 100 ms", ms);
}

void checkSignalDeath() {
  LivePool live;
  if (!live)
    return;
  pid_t pid = forkChild([] {
    ::raise(SIGTERM);
    return 0;
  });
  auto status = watchTarget(live.pool(), pid);
  check(status && WIFSIGNALED(*status) && WTERMSIG(*status) == SIGTERM &&
            exitCodeFor(*status) == 128 + SIGTERM,
        "exit", "death by SIGTERM comes back as that signal, code %d",
        128 + SIGTERM);
}

void checkStopsAndCloses() {
  LivePool live;
  if (!live)
    return;
  const auto &stopping = live.pool().header->watcher.liveness.stopping;
  bool clearBefore = stopping.load(std::memory_order_acquire) == 0;
  std::vector<int> before = openFds();
  pid_t pid = forkChild([] { return 0; });
  auto status = watchTarget(live.pool(), pid);
  std::vector<int> after = openFds();
  check(status && clearBefore && stopping.load(std::memory_order_acquire) == 1,
        "stop", "stopping is 0 before the loop and 1 once watchTarget returns");
  check(before == after, "stop",
        "the pidfd is closed (%zu fds before, %zu after)", before.size(),
        after.size());
}

volatile std::sig_atomic_t alarms = 0;
void countAlarm(int) { alarms = alarms + 1; }

void checkInterrupted() {
  LivePool live;
  if (!live)
    return;
  // No SA_RESTART, so every SIGALRM makes poll fail with EINTR.
  struct sigaction handler{}, old{};
  handler.sa_handler = countAlarm;
  ::sigaction(SIGALRM, &handler, &old);
  alarms = 0;

  pid_t pid = forkChild([&] { return heartbeatStaysFresh(live.pool(), 300); });
  // After the fork: timers aren't inherited, but set it here regardless so
  // only the watcher is interrupted.
  itimerval every{{0, 1000}, {0, 1000}}, off{};
  ::setitimer(ITIMER_REAL, &every, nullptr);
  auto status = watchTarget(live.pool(), pid);
  ::setitimer(ITIMER_REAL, &off, nullptr);
  ::sigaction(SIGALRM, &old, nullptr);

  int seen = alarms;
  check(status && exitCodeFor(*status) == 0 && seen > 10, "eintr",
        "%d interrupting signals, the loop kept going and the heartbeat "
        "stayed fresh",
        seen);
}

void checkNoProcess() {
  LivePool live;
  if (!live)
    return;
  // A pid that just stopped existing: our child, already collected.
  pid_t gone = forkChild([] { return 0; });
  waitFor(gone);
  std::vector<int> before = openFds();
  auto status = watchTarget(live.pool(), gone);
  std::vector<int> after = openFds();
  check(!status && std::string_view(status.error().step) == "pidfd_open" &&
            status.error().error == ESRCH,
        "error", "a pid with no process is pidfd_open with ESRCH (%s)",
        status ? "returned a status" : std::strerror(status.error().error));
  check(before == after, "error", "and leaves no fd open");
}

void checkRealProgram() {
  LivePool live;
  if (!live)
    return;
  Environment env = buildEnvironment(environ, TA_SHIM_PATH, live.fd());
  std::string argv0 = "/bin/sh", flag = "-c", script = "sleep 0.5";
  char *program[] = {argv0.data(), flag.data(), script.data(), nullptr};
  auto start = std::chrono::steady_clock::now();
  auto pid = spawnTarget(program, env.envp(), live.fd());
  auto status = pid ? watchTarget(live.pool(), *pid)
                    : std::expected<int, WatchError>(
                          std::unexpected(WatchError{"spawn", 0}));
  long long ms = msSince(start);
  std::uint32_t attached = attachedPid(live.pool());
  check(pid && status && exitCodeFor(*status) == 0 &&
            attached == static_cast<std::uint32_t>(*pid),
        "real", "sh -c 'sleep 0.5' through the loop: exit 0, shim attached");
  check(status && ms >= 500 && ms < 900, "real",
        "returned after %lld ms, as soon as the program ended", ms);
}

} // namespace

int main() {
  checkHeartbeat();
  checkPromptExit();
  checkSignalDeath();
  checkStopsAndCloses();
  checkInterrupted();
  checkNoProcess();
  checkRealProgram();
  return finish();
}
