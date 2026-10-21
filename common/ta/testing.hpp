#pragma once

// helpers for the self-checking test executables: each check prints one
// PASS/FAIL/SKIP line, and main ends with `return ta::test::finish();`, which
// exits nonzero if any check failed. no framework, so ctest and a bare run
// read the same output.

#include "ta/contracts.hpp"

#include <cstdarg>
#include <cstdio>
#include <string_view>

#if defined(__linux__) || defined(__APPLE__)
#include <csignal>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ta::test {

inline int failures = 0;

// for wrappers with their own status rules, e.g. the harness's checkTiming.
inline void report(const char *status, const char *name, const char *fmt,
                   va_list args) {
  std::printf("%s  %-9s ", status, name);
  std::vprintf(fmt, args);
  std::printf("\n");
}

[[gnu::format(printf, 3, 4)]] inline void check(bool ok, const char *name,
                                                const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  report(ok ? "PASS" : "FAIL", name, fmt, args);
  va_end(args);
  if (!ok)
    ++failures;
}

// a check that can't run in this build or environment; never a failure.
[[gnu::format(printf, 2, 3)]] inline void skip(const char *name,
                                               const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  report("SKIP", name, fmt, args);
  va_end(args);
}

// prints the summary line; the result is main's exit code.
inline int finish() {
  std::printf("%s: %d check(s) failed\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}

// a violated TA_PRE/TA_POST aborts only under these semantics; clang has no
// contracts at all, so they vanish.
constexpr bool contractsAbort() {
  std::string_view semantic = TA_CONTRACT_SEMANTIC;
  return TA_HAS_CONTRACTS &&
         (semantic == "enforce" || semantic == "quick_enforce");
}

// TA_ASSERT also aborts through its fallback in builds without NDEBUG.
constexpr bool assertsAbort() {
#if TA_HAS_CONTRACTS
  return contractsAbort();
#elif defined(NDEBUG)
  return false;
#else
  return true;
#endif
}

#if defined(__linux__) || defined(__APPLE__)
// runs f in a forked child, so a violation ends only the child. stderr is
// silenced (violation reports would drown the output) and core dumps are off
// (systemd-coredump stores one per abort, which takes seconds under mimalloc's
// large reservations). true if the child died of SIGABRT, which is how
// enforce, quick_enforce and the fallbacks all end.
template <class F> bool abortsInChild(F f) {
  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    rlimit noCore{0, 0};
    ::setrlimit(RLIMIT_CORE, &noCore);
    int devNull = ::open("/dev/null", O_WRONLY);
    if (devNull >= 0)
      ::dup2(devNull, STDERR_FILENO);
    f();
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif

} // namespace ta::test
