// Checks the whole launcher, main.cpp included, by running tieralloc-trace on
// launch_probe, which reports from inside the traced process. Covers the
// target's view (the pool fd closed, the shim first, the pool mapped and
// claimed by the target's pid, a live heartbeat for several timeouts, and
// nothing in a grandchild), main's exit codes and messages (the program's
// code, signal deaths, a missing program, usage errors with nothing run), an
// existing LD_PRELOAD kept after the shim, SIGTERM and SIGINT sent to the
// launcher, the smallest pool, the "never loaded the shim" warning (with a
// static probe, when one could be built), the shim's messages without the
// launcher, and errno at main unchanged by the shim. ctest runs it once plain
// and once per preloaded allocator, which the launcher and the probe inherit.
// Exits nonzero if any check fails.

#include "launch_probe.hpp"
#include "ta/testing.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace ta::test;
using namespace tatrace::probe;
using Clock = std::chrono::steady_clock;

const std::vector<std::string> smallPool = {"--rings", "2", "--ring-capacity",
                                            "1024"};

// The LD_PRELOAD this test was started with: an allocator under test, or
// nothing. Read once, before any child changes its own copy.
std::string inheritedPreload() {
  const char *value = std::getenv("LD_PRELOAD");
  return value ? value : "";
}

// The shim first, then whatever was preloaded, as buildEnvironment does.
std::string shimPreload() {
  std::string preload = TA_SHIM_PATH;
  if (std::string inherited = inheritedPreload(); !inherited.empty())
    preload += ":" + inherited;
  return preload;
}

struct Env {
  std::vector<std::pair<std::string, std::string>> set{};
  std::vector<std::string> unset{};
};

// In a forked child: apply env, then exec program with args. Never returns.
[[noreturn]] void execWith(const std::string &program,
                           const std::vector<std::string> &args,
                           const Env &env) {
  for (const auto &[name, value] : env.set)
    ::setenv(name.c_str(), value.c_str(), 1);
  for (const std::string &name : env.unset)
    ::unsetenv(name.c_str());
  std::vector<char *> argv;
  argv.push_back(const_cast<char *>(program.c_str()));
  for (const std::string &arg : args)
    argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  ::execv(program.c_str(), argv.data());
  ::_exit(127);
}

std::string readAll(int fd) {
  std::string text;
  ::lseek(fd, 0, SEEK_SET);
  char buffer[4096];
  for (;;) {
    ssize_t n = ::read(fd, buffer, sizeof buffer);
    if (n == -1 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    text.append(buffer, static_cast<std::size_t>(n));
  }
  return text;
}

struct Result {
  int code = -1;
  std::string out, err;
  long long ms = 0;
};

// Runs program to the end. stdout and stderr go to memfds rather than
// pipes: a child that filled one pipe while we blocked reading the other
// would deadlock, and a file never fills.
Result run(const std::string &program, const std::vector<std::string> &args,
           const Env &env = {}) {
  Result r;
  int out = ::memfd_create("launch-out", MFD_CLOEXEC);
  int err = ::memfd_create("launch-err", MFD_CLOEXEC);
  auto start = Clock::now();
  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid == 0) {
    // dup2 clears close-on-exec on the copies
    ::dup2(out, STDOUT_FILENO);
    ::dup2(err, STDERR_FILENO);
    execWith(program, args, env);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
  }
  r.ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                               start)
             .count();
  r.code = WIFEXITED(status)     ? WEXITSTATUS(status)
           : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                 : -1;
  r.out = readAll(out);
  r.err = readAll(err);
  ::close(out);
  ::close(err);
  return r;
}

Result launch(std::vector<std::string> options,
              const std::vector<std::string> &program, const Env &env = {}) {
  std::vector<std::string> args{"run"};
  args.insert(args.end(), options.begin(), options.end());
  args.emplace_back("--");
  args.insert(args.end(), program.begin(), program.end());
  return run(TA_LAUNCHER, args, env);
}

// Lines starting with prefix.
int linesWith(std::string_view text, std::string_view prefix) {
  int lines = 0;
  for (std::size_t at = 0; at < text.size();) {
    std::size_t end = text.find('\n', at);
    if (end == std::string_view::npos)
      end = text.size();
    if (text.substr(at, end - at).starts_with(prefix))
      ++lines;
    at = end + 1;
  }
  return lines;
}

bool has(const std::string &text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

void checkTargetView() {
  Result r = launch(smallPool, {TA_PROBE, "attached"});
  check(r.code == Passed && linesWith(r.err, "tatrace:") == 0 &&
            linesWith(r.err, "tieralloc-trace:") == 0,
        "target",
        "the pool fd is closed, the shim is first and the pool is mapped, "
        "with nothing on stderr (code %d)",
        r.code);

  // 5x the timeout: a watcher that stopped beating would be caught
  std::vector<std::string> shortTimeout = smallPool;
  shortTimeout.insert(shortTimeout.end(), {"--heartbeat-timeout-ms", "200"});
  r = launch(shortTimeout, {TA_PROBE, "pool", "1000"});
  check(r.code == Passed && r.ms < 1500, "target",
        "the target sees its own pid as attachedPid and a heartbeat under "
        "50 ms old for 1 s (code %d, %lld ms)",
        r.code, r.ms);

  r = launch(smallPool, {TA_PROBE, "grandchild"});
  check(r.code == Passed && linesWith(r.err, "tatrace:") == 0, "target",
        "an exec'd grandchild sees no pool and prints nothing (code %d)",
        r.code);

  r = launch({"--rings", "1", "--ring-capacity", "64"}, {TA_PROBE, "attached"});
  check(r.code == Passed, "target",
        "the smallest pool, 1 ring of 64 slots, works end to end (code %d)",
        r.code);
}

// The probe reads the pool after its threads are joined; nothing recycles
// rings yet, so every closed ring is still there to inspect.
void checkThreadRings() {
  Result r = launch({"--rings", "16", "--ring-capacity", "1024"},
                    {TA_PROBE, "threads", "8", "0"});
  check(r.code == Passed && linesWith(r.err, "tatrace:") == 0, "threads",
        "8 threads each claim a ring holding ThreadStart then ThreadExit and "
        "close it, and main's ring stays Active (code %d)",
        r.code);

  r = launch(smallPool, {TA_PROBE, "threads", "4", "3"});
  check(r.code == Passed, "threads",
        "with 2 rings, main and one thread claim and the other 3 threads are "
        "disabled (code %d)",
        r.code);

  r = launch(smallPool, {TA_PROBE, "fork-threads"});
  check(r.code == Passed, "threads",
        "a forked child and its thread claim no ring, and main's ring stays "
        "Active (code %d)",
        r.code);

  r = launch(smallPool, {TA_PROBE, "late"});
  check(r.code == Passed, "threads",
        "a key destructor's malloc and free after the ring closed count as "
        "exactly 2 late events (code %d)",
        r.code);
}

void checkExitCodes() {
  Result r = launch(smallPool, {TA_PROBE, "exit", "3"});
  check(r.code == 3 && linesWith(r.err, "tieralloc-trace:") == 0, "exit",
        "the program's exit 3 is the launcher's, silently (code %d)", r.code);

  r = launch(smallPool, {TA_PROBE, "raise", "TERM"});
  check(r.code == 128 + SIGTERM && has(r.err, "killed by signal 15"), "exit",
        "a SIGTERM death gives 143 and names the signal (code %d)", r.code);

  r = launch(smallPool, {"/nonexistent/prog"});
  check(r.code == 127 && has(r.err, "cannot run /nonexistent/prog") &&
            has(r.err, std::strerror(ENOENT)),
        "exit", "a missing program gives 127 and 'cannot run' (code %d)",
        r.code);

  struct Usage {
    const char *what;
    std::vector<std::string> args;
  };
  const Usage cases[] = {
      {"--rings 0", {"run", "--rings", "0", "--", TA_PROBE, "env"}},
      {"no --", {"run", TA_PROBE, "env"}},
      {"--shim /nonexistent",
       {"run", "--shim", "/nonexistent/shim.so", "--", TA_PROBE, "env"}},
  };
  for (const Usage &u : cases) {
    Result bad = run(TA_LAUNCHER, u.args);
    check(bad.code == 2 && bad.err.starts_with("tieralloc-trace:") &&
              has(bad.err, "usage:") && bad.out.empty(),
          "usage",
          "%s: exit 2 with the usage, and the probe never ran (code %d)",
          u.what, bad.code);
  }
}

void checkPreloadKept() {
  Result r = launch(smallPool, {TA_PROBE, "env"},
                    {.set = {{"LD_PRELOAD", "/nonexistent/other.so"}}});
  // the loader warns about the missing file on stderr; that isn't ours
  std::string want = std::string(TA_SHIM_PATH) + ":/nonexistent/other.so\n";
  check(r.code == Passed && r.out == want, "preload",
        "an existing LD_PRELOAD is kept after the shim (got '%.*s')",
        static_cast<int>(r.out.empty() ? 0 : r.out.size() - 1), r.out.c_str());

  r = launch(smallPool, {TA_PROBE, "env"});
  check(r.code == Passed && r.out == shimPreload() + "\n", "preload",
        "the inherited preload ('%s') runs under the shim",
        inheritedPreload().c_str());
}

// The launcher on 'probe sleep 5000', sent sig once the probe says ready.
void checkSignal(int sig) {
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) == -1) {
    check(false, "signal", "pipe2 failed");
    return;
  }
  std::vector<std::string> args{"run"};
  args.insert(args.end(), smallPool.begin(), smallPool.end());
  args.insert(args.end(), {"--", TA_PROBE, "sleep", "5000"});
  auto start = Clock::now();
  std::fflush(nullptr);
  pid_t launcher = ::fork();
  if (launcher == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    int devNull = ::open("/dev/null", O_WRONLY);
    ::dup2(devNull, STDERR_FILENO);
    execWith(TA_LAUNCHER, args, {});
  }
  ::close(fds[1]);
  std::string out;
  char c;
  while (out.find('\n') == std::string::npos) {
    ssize_t n = ::read(fds[0], &c, 1);
    if (n == -1 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    out += c;
  }
  ::kill(launcher, sig);
  int status = 0;
  while (::waitpid(launcher, &status, 0) == -1 && errno == EINTR) {
  }
  ::close(fds[0]);
  long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     Clock::now() - start)
                     .count();
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  check(out == "ready\n" && code == 128 + sig && ms < 3000, "signal",
        "SIG%s to the launcher once the probe is running: forwarded, exit %d "
        "after %lld ms",
        sigabbrev_np(sig), code, ms);
}

void checkNeverLoaded() {
  std::string probe = TA_PROBE_STATIC;
  if (probe.empty()) {
    skip("static",
         "no static probe: linking with -static needs glibc-static and "
         "libstdc++-static");
    return;
  }
  Result r = launch(smallPool, {probe, "exit", "0"});
  check(r.code == 0 && has(r.err, "never loaded the shim"), "static",
        "a static program can't load the shim, and the launcher says so "
        "(code %d)",
        r.code);
}

// The shim preloaded without the launcher, on the inherited allocator.
Result preloadedOnly(const Env &extra, const std::vector<std::string> &args) {
  Env env = extra;
  env.set.emplace_back("LD_PRELOAD", shimPreload());
  return run(TA_PROBE, args, env);
}

void checkShimMessages() {
  Result r = preloadedOnly({.unset = {"TATRACE_FD"}}, {"exit", "7"});
  check(r.code == 7 && linesWith(r.err, "tatrace:") == 1 &&
            has(r.err, "not started by"),
        "shim", "without the launcher: one 'not started by' line (code %d)",
        r.code);

  r = preloadedOnly({.set = {{"TATRACE_FD", "abc"}}}, {"exit", "7"});
  check(r.code == 7 && linesWith(r.err, "tatrace:") == 1 &&
            has(r.err, "not a file descriptor number"),
        "shim",
        "TATRACE_FD=abc: one 'not a file descriptor number' line "
        "(code %d)",
        r.code);

  r = preloadedOnly({.set = {{"TATRACE_FD", "999"}}}, {"exit", "7"});
  check(r.code == 7 && linesWith(r.err, "tatrace:") == 0, "shim",
        "TATRACE_FD naming a closed fd is silent (code %d)", r.code);
}

// The shim promises errno unchanged, not zero: some allocators set it before
// main on their own, so compare against the run without the shim.
void checkErrno() {
  Result plain = run(TA_PROBE, {"errno"}, {.unset = {"TATRACE_FD"}});
  Result traced = launch(smallPool, {TA_PROBE, "errno"});
  Result preloaded = preloadedOnly({.unset = {"TATRACE_FD"}}, {"errno"});
  bool ok = plain.code == Passed && traced.code == Passed &&
            preloaded.code == Passed && !plain.out.empty() &&
            plain.out == traced.out && plain.out == preloaded.out;
  auto value = [](const std::string &s) {
    return s.empty() ? std::string("?") : s.substr(0, s.size() - 1);
  };
  check(ok, "errno",
        "errno at main is the same untraced (%s), traced (%s) and preloaded "
        "without the launcher (%s)",
        value(plain.out).c_str(), value(traced.out).c_str(),
        value(preloaded.out).c_str());
}

} // namespace

int main() {
  checkTargetView();
  checkThreadRings();
  checkExitCodes();
  checkPreloadKept();
  checkSignal(SIGTERM);
  checkSignal(SIGINT);
  checkNeverLoaded();
  checkShimMessages();
  checkErrno();
  return finish();
}
