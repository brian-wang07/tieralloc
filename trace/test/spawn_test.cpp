// Checks for the launcher's spawning (src/launcher/spawn.hpp). buildEnvironment
// is checked as a pure function: the shim leads LD_PRELOAD, TATRACE_FD is
// replaced, lookalikes and everything else pass through, and the pointer list
// stays valid after a move. exitCodeFor is checked on real children's
// statuses. spawnTarget runs real programs against a real pool with the
// build's shim: exit codes and signals come back through waitFor, the shim
// attaches in the target, the target holds the pool mapped but not its fd,
// spawnTarget returns once exec succeeds rather than when the program ends,
// each failure stage is reported with its errno and the failed child is
// collected, and no fd leaks into the launcher. Exits nonzero if any check
// fails.

#include "launcher/spawn.hpp"
#include "ta/testing.hpp"
#include "tatrace/pool.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace tatrace;
using namespace tatrace::launcher;
using namespace ta::test;

constexpr std::string_view testShim = "/s/libtieralloc-trace.so";

// A writable, null-terminated list of strings, as environ or argv is.
class StringList {
public:
  StringList(std::initializer_list<const char *> items) {
    for (const char *item : items)
      storage.emplace_back(item);
    for (std::string &s : storage)
      pointers.push_back(s.data());
    pointers.push_back(nullptr);
  }
  StringList(const StringList &) = delete;
  StringList &operator=(const StringList &) = delete;

  char **get() { return pointers.data(); }

private:
  std::vector<std::string> storage;
  std::vector<char *> pointers;
};

// envp's entries, in order, without the trailing nullptr.
std::vector<std::string_view> entries(const Environment &env) {
  std::vector<std::string_view> out;
  for (char *const *entry = env.envp(); *entry != nullptr; ++entry)
    out.emplace_back(*entry);
  return out;
}

bool entriesAre(const Environment &env,
                std::initializer_list<std::string_view> want) {
  return entries(env) == std::vector<std::string_view>(want);
}

// Every envp pointer is the data of the matching owned string, and the list
// ends right after them.
bool pointersOwned(const Environment &env) {
  const std::vector<std::string> &vars = env.variables();
  char *const *envp = env.envp();
  for (std::size_t i = 0; i < vars.size(); ++i)
    if (envp[i] != vars[i].data())
      return false;
  return envp[vars.size()] == nullptr;
}

void checkEnvironment() {
  StringList full{"HOME=/h",         "LD_PRELOAD=/j.so", "TATRACE_FD=99",
                  "X=LD_PRELOAD=no", "LD_PRELOAD_X=1",   "PATH=/bin"};
  Environment env = buildEnvironment(full.get(), testShim, 5);
  check(entriesAre(
            env, {"HOME=/h", "X=LD_PRELOAD=no", "LD_PRELOAD_X=1", "PATH=/bin",
                  "LD_PRELOAD=/s/libtieralloc-trace.so:/j.so", "TATRACE_FD=5"}),
        "env",
        "the shim leads the old LD_PRELOAD, TATRACE_FD is replaced, and "
        "lookalikes and the rest pass through in order");
  check(pointersOwned(env), "env", "every envp pointer is an owned string");

  StringList none{"HOME=/h"};
  check(entriesAre(
            buildEnvironment(none.get(), testShim, 7),
            {"HOME=/h", "LD_PRELOAD=/s/libtieralloc-trace.so", "TATRACE_FD=7"}),
        "env", "without LD_PRELOAD, it's added as just the shim");

  StringList empty{"LD_PRELOAD=", "HOME=/h"};
  check(entriesAre(
            buildEnvironment(empty.get(), testShim, 7),
            {"HOME=/h", "LD_PRELOAD=/s/libtieralloc-trace.so", "TATRACE_FD=7"}),
        "env", "an empty LD_PRELOAD leaves no stray ':'");

  Environment null = buildEnvironment(nullptr, testShim, 3);
  check(entriesAre(null,
                   {"LD_PRELOAD=/s/libtieralloc-trace.so", "TATRACE_FD=3"}) &&
            pointersOwned(null),
        "env", "a null environment gives just the two variables");

  // Enough variables that the strings live on the heap and short ones in
  // the string objects, so a broken move would show either way.
  StringList many{"A=1", "B=a-value-long-enough-to-not-fit-in-sso-storage",
                  "C=3", "D=4",
                  "E=5", "F=6",
                  "G=7", "H=8"};
  Environment original = buildEnvironment(many.get(), testShim, 9);
  std::vector<std::string> before(original.variables());
  Environment moved = std::move(original);
  std::vector<std::string_view> after = entries(moved);
  check(pointersOwned(moved) && std::vector<std::string_view>(
                                    before.begin(), before.end()) == after,
        "env", "after a move, the pointers are the new owner's strings");
}

// The raw status of a child that runs f and exits with its result.
template <class F> int statusOf(F f) {
  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid == 0)
    ::_exit(f());
  return waitFor(pid);
}

void checkExitCode() {
  int exited = statusOf([] { return 3; });
  check(exitCodeFor(exited) == 3, "status", "exit(3) gives 3");
  int zero = statusOf([] { return 0; });
  check(exitCodeFor(zero) == 0, "status", "exit(0) gives 0");
  int termed = statusOf([] {
    ::raise(SIGTERM);
    return 0;
  });
  check(exitCodeFor(termed) == 128 + SIGTERM, "status",
        "death by SIGTERM gives %d (got %d)", 128 + SIGTERM,
        exitCodeFor(termed));
  int killed = statusOf([] {
    ::raise(SIGKILL);
    return 0;
  });
  check(exitCodeFor(killed) == 128 + SIGKILL, "status",
        "death by SIGKILL gives %d (got %d)", 128 + SIGKILL,
        exitCodeFor(killed));
}

// The fds this process has open, by number.
std::vector<int> openFds() {
  std::vector<int> fds;
  for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd"))
    fds.push_back(std::atoi(entry.path().filename().c_str()));
  // the iterator's own directory fd is in the listing; it's closed now
  std::erase_if(fds, [](int fd) { return ::fcntl(fd, F_GETFD) == -1; });
  return fds;
}

// A real pool and an environment that preloads the build's shim into it,
// destroyed on every path out of a check.
class Launch {
public:
  Launch() : created(createPool({2, 1024, defaultHeartbeatTimeoutNs})) {
    if (!created)
      check(false, "spawn", "createPool failed at %s: %s", created.error().step,
            std::strerror(created.error().error));
    else
      env = buildEnvironment(environ, TA_SHIM_PATH, created->fd);
  }
  Launch(const Launch &) = delete;
  Launch &operator=(const Launch &) = delete;
  ~Launch() {
    if (created)
      destroyPool(*created);
  }

  explicit operator bool() const { return created.has_value(); }
  int fd() const { return created->fd; }
  const Pool &pool() const { return created->pool; }
  std::expected<pid_t, SpawnError>
  spawn(std::initializer_list<const char *> argv, int poolFd) {
    StringList program(argv);
    return spawnTarget(program.get(), env.envp(), poolFd);
  }
  std::expected<pid_t, SpawnError>
  spawn(std::initializer_list<const char *> argv) {
    return spawn(argv, fd());
  }

private:
  std::expected<CreatedPool, PoolError> created;
  Environment env;
};

// The program's exit code, or -1 if spawnTarget failed.
int runToEnd(Launch &launch, std::initializer_list<const char *> argv) {
  auto pid = launch.spawn(argv);
  return pid ? exitCodeFor(waitFor(*pid)) : -1;
}

void checkSpawnStatus() {
  Launch launch;
  if (!launch)
    return;
  check(runToEnd(launch, {"/bin/true"}) == 0, "spawn", "/bin/true gives 0");
  check(runToEnd(launch, {"true"}) == 0, "spawn",
        "a bare name is found through PATH");
  check(runToEnd(launch, {"/bin/sh", "-c", "exit 3"}) == 3, "spawn",
        "sh -c 'exit 3' gives 3");
  check(runToEnd(launch, {"/bin/sh", "-c", "kill -TERM $$"}) == 128 + SIGTERM,
        "spawn", "a program killed by SIGTERM gives %d", 128 + SIGTERM);
  check(runToEnd(launch,
                 {"/bin/sh", "-c", "test \"$1$2\" = ab", "sh", "a", "b"}) == 0,
        "spawn", "the program's arguments arrive as given");
}

void checkAttached() {
  Launch launch;
  if (!launch)
    return;
  auto pid = launch.spawn({"/bin/true"});
  int code = pid ? exitCodeFor(waitFor(*pid)) : -1;
  std::uint32_t attached = attachedPid(launch.pool());
  check(pid && code == 0 && attached == static_cast<std::uint32_t>(*pid),
        "attach", "the shim attached in the target (attachedPid %u, pid %d)",
        attached, pid ? *pid : -1);

  // A second launch on a fresh pool: the target has the pool mapped, the fd
  // it inherited was closed by the attach, and LD_PRELOAD starts with the
  // shim. "; exit" keeps sh from exec'ing the last command in its place.
  Launch inside;
  if (!inside)
    return;
  std::string script =
      std::string("grep -q memfd:") + poolMemfdName +
      " /proc/$$/maps || exit 10; "
      "ls -l /proc/$$/fd | grep -q memfd: && exit 11; "
      "case \"$LD_PRELOAD\" in \"$1\"*) ;; *) exit 12;; esac; exit 0";
  std::string shim(TA_SHIM_PATH);
  auto inner =
      inside.spawn({"/bin/sh", "-c", script.c_str(), "sh", shim.c_str()});
  int innerCode = inner ? exitCodeFor(waitFor(*inner)) : -1;
  check(innerCode == 0, "attach",
        "the target maps the pool, holds no pool fd, and sees the shim "
        "first in LD_PRELOAD (code %d; 10 not mapped, 11 fd open, 12 "
        "LD_PRELOAD)",
        innerCode);
}

void checkReturnsAtExec() {
  Launch launch;
  if (!launch)
    return;
  auto start = std::chrono::steady_clock::now();
  auto pid = launch.spawn({"/bin/sleep", "0.5"});
  auto took = std::chrono::steady_clock::now() - start;
  if (pid)
    waitFor(*pid);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(took).count();
  // A status pipe end leaking into the program would hold off EOF until the
  // program exited.
  check(pid && ms < 250, "spawn",
        "spawnTarget returns once the exec succeeds, not when the program "
        "ends (%lld ms for a 500 ms program)",
        static_cast<long long>(ms));
}

// True if this process has no children left to collect.
bool noChildren() {
  int status = 0;
  return ::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD;
}

void checkFailures() {
  Launch launch;
  if (!launch)
    return;

  auto missing = launch.spawn({"/nonexistent/prog"});
  check(!missing && missing.error().kind == SpawnError::Kind::Exec &&
            std::string_view(missing.error().step) == "execvpe" &&
            missing.error().error == ENOENT,
        "fail", "a missing program is Exec/execvpe with ENOENT");
  check(noChildren(), "fail", "the failed child was collected");

  auto notExec = launch.spawn({"/etc/passwd"});
  check(!notExec && notExec.error().kind == SpawnError::Kind::Exec &&
            notExec.error().error == EACCES,
        "fail", "a file that isn't executable is Exec with EACCES");

  // A closed fd makes the child's fcntl fail before it gets to exec.
  int closed = ::dup(launch.fd());
  ::close(closed);
  auto badFd = launch.spawn({"/bin/true"}, closed);
  check(!badFd && badFd.error().kind == SpawnError::Kind::Exec &&
            std::string_view(badFd.error().step) == "fcntl" &&
            badFd.error().error == EBADF,
        "fail", "a bad pool fd is reported as the fcntl stage with EBADF");
  check(noChildren(), "fail", "and that child was collected too");
}

void checkNoLeaks() {
  Launch launch;
  if (!launch)
    return;
  std::vector<int> before = openFds();
  auto ok = launch.spawn({"/bin/true"});
  if (ok)
    waitFor(*ok);
  auto failed = launch.spawn({"/nonexistent/prog"});
  std::vector<int> after = openFds();
  check(ok && !failed && before == after, "fds",
        "spawnTarget leaves no fd open in the launcher (%zu before, %zu "
        "after)",
        before.size(), after.size());
  check(::fcntl(launch.fd(), F_GETFD) == FD_CLOEXEC, "fds",
        "the launcher's own pool fd keeps FD_CLOEXEC");
}

} // namespace

int main() {
  checkEnvironment();
  checkExitCode();
  checkSpawnStatus();
  checkAttached();
  checkReturnsAtExec();
  checkFailures();
  checkNoLeaks();
  return finish();
}
