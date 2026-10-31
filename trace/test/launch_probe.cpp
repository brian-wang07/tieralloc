// The program tieralloc-trace launches in launch_test. It's the only code
// that runs inside the traced process, so it checks what no outside test can
// see: the pool fd closed, the shim first in LD_PRELOAD, the pool mapped and
// claimed by this pid, a live heartbeat, and no pool in a grandchild, and
// each thread's ring: claimed at its first event, closed at its exit, never
// claimed by a forked child. Other modes behave predictably on request (exit,
// raise, sleep, print). The exit code and stdout are its report; launch_test
// judges them. Nothing may run before main, since errno at main's first line
// is part of what it reports, so no <iostream> and no globals with
// constructors. It only reads the pool, and never moves a ring's tail.

#include "launch_probe.hpp"
#include "ta/ticks.hpp"
#include "tatrace/format.hpp"
#include "tatrace/pool.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

using namespace tatrace;
using namespace tatrace::probe;

constexpr std::string_view shimName = "libtieralloc-trace.so";
// A heartbeat older than this counts as stale: 50 of the watcher's intervals.
constexpr std::int64_t staleNs = 50'000'000;

void sleepMs(long ms) {
  timespec ts{ms / 1000, (ms % 1000) * 1'000'000};
  while (::nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
}

// The start of this process's /memfd:tatrace-pool mapping, or nullptr. The
// kernel appends " (deleted)", so match the name rather than the line's end.
const void *findPoolMapping() {
  std::FILE *maps = std::fopen("/proc/self/maps", "r");
  if (maps == nullptr)
    return nullptr;
  char needle[64];
  std::snprintf(needle, sizeof needle, "/memfd:%s", poolMemfdName);
  char line[512];
  const void *start = nullptr;
  while (start == nullptr && std::fgets(line, sizeof line, maps) != nullptr)
    if (std::strstr(line, needle) != nullptr)
      start = reinterpret_cast<const void *>(
          static_cast<std::uintptr_t>(std::strtoull(line, nullptr, 16)));
  std::fclose(maps);
  return start;
}

// The first ':'-separated entry of LD_PRELOAD is a file named like the shim.
bool shimIsFirst() {
  const char *preload = std::getenv("LD_PRELOAD");
  if (preload == nullptr)
    return false;
  std::string_view first = preload;
  first = first.substr(0, first.find(':'));
  std::size_t slash = first.rfind('/');
  return first.substr(slash == std::string_view::npos ? 0 : slash + 1) ==
         shimName;
}

int signalByName(std::string_view name) {
  if (name == "TERM")
    return SIGTERM;
  if (name == "INT")
    return SIGINT;
  if (name == "HUP")
    return SIGHUP;
  if (name == "QUIT")
    return SIGQUIT;
  if (name == "KILL")
    return SIGKILL;
  if (name == "SEGV")
    return SIGSEGV;
  return 0;
}

int attached() {
  // first, before any file is opened: the next open would most likely be
  // given the closed fd's number and make it look open again
  int fd = parsePoolFd(std::getenv(poolFdEnv));
  if (fd < 0 || ::fcntl(fd, F_GETFD) != -1 || errno != EBADF)
    return FdStillOpen;
  if (!shimIsFirst())
    return ShimNotFirst;
  if (findPoolMapping() == nullptr)
    return NoPoolMapping;
  return Passed;
}

int pool(long ms) {
  const auto *header = static_cast<const PoolHeader *>(findPoolMapping());
  if (header == nullptr)
    return NoPoolMapping;
  if (std::atomic_ref(header->fixed.magic).load(std::memory_order_acquire) !=
      poolMagic)
    return BadMagic;
  if (header->shim.attachedPid.load(std::memory_order_acquire) !=
      static_cast<std::uint32_t>(::getpid()))
    return NotAttachedPid;
  const auto &heartbeat = header->watcher.liveness.heartbeatNs;
  std::int64_t end = ta::monotonicNs() + ms * 1'000'000;
  while (ta::monotonicNs() < end) {
    // loaded before the clock is read, so the age can't come out negative
    auto beat =
        static_cast<std::int64_t>(heartbeat.load(std::memory_order_relaxed));
    if (ta::monotonicNs() - beat > staleNs)
      return StaleHeartbeat;
    sleepMs(5);
  }
  return Passed;
}

int notAttached() { return findPoolMapping() == nullptr ? Passed : PoolMapped; }

// exec, not just fork: only a fresh process runs the shim's init, which is
// the real grandchild path (TATRACE_FD names a closed fd).
int grandchild(const char *self) {
  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid == -1)
    return ExecFailed;
  if (pid == 0) {
    char mode[] = "not-attached";
    char *argv[] = {const_cast<char *>(self), mode, nullptr};
    ::execv("/proc/self/exe", argv);
    ::_exit(ExecFailed);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
  }
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return WEXITSTATUS(status);
}

// Written through a volatile pointer so the compiler can't drop a
// malloc/free pair, which GCC does at -O2.
void *volatile sink = nullptr;

void allocateAndFree() {
  sink = std::malloc(64);
  std::free(sink);
}

void *threadBody(void *) {
  allocateAndFree();
  return nullptr;
}

// A Pool handle over the shim's mapping of the pool in this process.
std::optional<Pool> attachedPool() {
  auto *header =
      static_cast<PoolHeader *>(const_cast<void *>(findPoolMapping()));
  if (header == nullptr)
    return std::nullopt;
  return Pool{header, reinterpret_cast<std::byte *>(header), 0};
}

// The records ring i has published, read in place. Thread rings in these
// modes hold a few records, far below capacity, so nothing has wrapped.
std::span<const Record> peekRing(const Pool &pool, std::uint32_t i) {
  RingControl *ring = pool.ring(i);
  std::uint64_t head = ring->producer.head.load(std::memory_order_acquire);
  return {detail::ringSlots(ring), std::min(head, ring->fixed.capacity)};
}

bool isThreadRecord(const Record &r, Kind kind, std::uint32_t tid) {
  return r.kind == kind && r.addr == tid;
}

std::uint32_t claimedRings(const Pool &pool) {
  std::uint32_t claimed = 0;
  for (std::uint32_t i = 0; i < pool.ringCount(); ++i)
    if (ringState(pool, i) != RingState::Free)
      ++claimed;
  return claimed;
}

// main's ring is Active and starts with ThreadStart(pid); exactly
// expectClosed rings are Closed, each holding just ThreadStart(tid) and
// ThreadExit(tid); no ring is in any other state; tids and serials are
// distinct.
int checkRings(const Pool &pool, std::uint32_t expectClosed) {
  auto pid = static_cast<std::uint32_t>(::getpid());
  bool mainActive = false;
  std::uint32_t closed = 0;
  std::vector<std::uint32_t> tids, serials;
  for (std::uint32_t i = 0; i < pool.ringCount(); ++i) {
    RingState state = ringState(pool, i);
    if (state == RingState::Free)
      continue;
    const PoolRingLine &line = *pool.ringLine(i);
    std::span<const Record> records = peekRing(pool, i);
    tids.push_back(line.tid);
    serials.push_back(line.serial);
    if (state == RingState::Active && line.tid == pid) {
      if (records.empty() ||
          !isThreadRecord(records[0], Kind::ThreadStart, pid))
        return MainNotActive;
      mainActive = true;
    } else if (state == RingState::Closed) {
      ++closed;
      if (records.size() != 2 ||
          !isThreadRecord(records[0], Kind::ThreadStart, line.tid) ||
          !isThreadRecord(records[1], Kind::ThreadExit, line.tid))
        return BadThreadRecords;
    } else {
      return StrayRing;
    }
  }
  if (!mainActive)
    return MainNotActive;
  if (closed != expectClosed)
    return WrongClosedCount;
  std::ranges::sort(tids);
  std::ranges::sort(serials);
  if (std::ranges::adjacent_find(tids) != tids.end() ||
      std::ranges::adjacent_find(serials) != serials.end())
    return DuplicateRing;
  return Passed;
}

// main claims first, then n threads each claim at their first malloc and
// close at exit; with too few rings, the rest are disabled.
int threads(std::uint32_t n, std::uint64_t expectDisabled) {
  allocateAndFree();
  std::vector<pthread_t> ids(n);
  for (pthread_t &id : ids)
    if (::pthread_create(&id, nullptr, threadBody, nullptr) != 0)
      return ThreadFailed;
  for (pthread_t id : ids)
    if (::pthread_join(id, nullptr) != 0)
      return ThreadFailed;
  std::optional<Pool> pool = attachedPool();
  if (!pool)
    return NoPoolMapping;
  if (expectDisabled > n)
    return BadUsage;
  if (int result =
          checkRings(*pool, n - static_cast<std::uint32_t>(expectDisabled));
      result != Passed)
    return result;
  if (disabledThreads(*pool) != expectDisabled)
    return WrongDisabled;
  if (lateEvents(*pool) != 0)
    return WrongLateEvents;
  return Passed;
}

// The child inherits main's claimed state, then allocates on its main thread
// and on a new thread; neither may claim, and main's ring must stay Active.
int forkThreads() {
  allocateAndFree();
  std::optional<Pool> pool = attachedPool();
  if (!pool)
    return NoPoolMapping;
  std::uint32_t before = claimedRings(*pool);
  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid == -1)
    return ForkFailed;
  if (pid == 0) {
    allocateAndFree();
    pthread_t id;
    if (::pthread_create(&id, nullptr, threadBody, nullptr) != 0 ||
        ::pthread_join(id, nullptr) != 0)
      ::_exit(ThreadFailed);
    ::_exit(Passed);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != Passed)
    return ForkFailed;
  if (claimedRings(*pool) != before)
    return ForkClaimed;
  return checkRings(*pool, 0);
}

pthread_key_t lateKey;

// Created after the shim's key, so in each round it runs after the shim's
// destructor: in the last round, after the ring has closed.
void lateDestructor(void *value) {
  allocateAndFree();
  ::pthread_setspecific(lateKey, value);
}

void *lateBody(void *) {
  allocateAndFree();
  ::pthread_setspecific(lateKey, &lateKey);
  return nullptr;
}

// The late key's last-round malloc and free come after the ring closed:
// exactly 2 late events, and the ring still holds just its two records.
int late() {
  if (::pthread_key_create(&lateKey, lateDestructor) != 0)
    return ThreadFailed;
  allocateAndFree();
  pthread_t id;
  if (::pthread_create(&id, nullptr, lateBody, nullptr) != 0 ||
      ::pthread_join(id, nullptr) != 0)
    return ThreadFailed;
  std::optional<Pool> pool = attachedPool();
  if (!pool)
    return NoPoolMapping;
  if (int result = checkRings(*pool, 1); result != Passed)
    return result;
  if (lateEvents(*pool) != 2)
    return WrongLateEvents;
  return Passed;
}

int usage(const char *what) {
  std::fprintf(stderr, "launch_probe: %s\n", what);
  return BadUsage;
}

} // namespace

int main(int argc, char **argv) {
  int startErrno = errno;
  if (argc < 2)
    return usage("no mode given");
  std::string_view mode = argv[1];
  const char *arg = argc > 2 ? argv[2] : nullptr;

  if (mode == "attached")
    return attached();
  if (mode == "pool")
    return arg ? pool(std::atol(arg)) : usage("pool needs <ms>");
  if (mode == "not-attached")
    return notAttached();
  if (mode == "grandchild")
    return grandchild(argv[0]);
  if (mode == "threads")
    return argc > 3 ? threads(static_cast<std::uint32_t>(std::atol(argv[2])),
                              std::strtoull(argv[3], nullptr, 10))
                    : usage("threads needs <n> <expectDisabled>");
  if (mode == "fork-threads")
    return forkThreads();
  if (mode == "late")
    return late();
  if (mode == "errno") {
    std::printf("%d\n", startErrno);
    return Passed;
  }
  if (mode == "env") {
    const char *preload = std::getenv("LD_PRELOAD");
    std::printf("%s\n", preload ? preload : "");
    return Passed;
  }
  if (mode == "exit")
    return arg ? std::atoi(arg) : usage("exit needs <n>");
  if (mode == "raise") {
    int sig = arg ? signalByName(arg) : 0;
    if (sig == 0)
      return usage("raise needs TERM, INT, HUP, QUIT, KILL or SEGV");
    std::raise(sig);
    return BadUsage;
  }
  if (mode == "sleep") {
    if (!arg)
      return usage("sleep needs <ms>");
    // stdout is a pipe in the signal checks, so fully buffered: flush, or
    // the test waits for a line that never arrives
    std::puts("ready");
    std::fflush(stdout);
    sleepMs(std::atol(arg));
    return Passed;
  }
  return usage("unknown mode");
}
