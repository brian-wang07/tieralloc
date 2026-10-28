#pragma once

// launches the traced program along with its environment, and turns its wait
// status into the correct exit code.

#include "launcher/signals.hpp"
#include "tatrace/pool.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tatrace::launcher {
inline constexpr int launcherErrorExit = 1;
inline constexpr int execFailedExit = 127;

class Environment {
public:
  Environment() = default;
  Environment(Environment &&) = default;
  Environment &operator=(Environment &&) = default;
  Environment(const Environment &) = delete;
  Environment &operator=(const Environment &) = delete;

  char *const *envp() const { return pointers.data(); }
  const std::vector<std::string> &variables() const { return storage; }

private:
  friend Environment buildEnvironment(char *const *, std::string_view, int);
  std::vector<std::string> storage;
  std::vector<char *> pointers;
};

inline Environment buildEnvironment(char *const *current, std::string_view shim,
                                    int poolFd) {
  constexpr std::string_view preloadKey = "LD_PRELOAD=";
  const std::string fdKey = std::string(poolFdEnv) + "=";

  Environment env;
  std::string oldPreload;
  // a process can be started with no environment at all
  if (current != nullptr) {
    for (char *const *entry = current; *entry != nullptr; ++entry) {
      std::string_view var = *entry;
      // the "=" is part of the key, so LD_PRELOAD_X and X=LD_PRELOAD= don't
      // match
      if (var.starts_with(preloadKey))
        oldPreload = var.substr(preloadKey.size());
      else if (!var.starts_with(fdKey))
        env.storage.emplace_back(var);
    }
  }

  std::string preload = std::string(preloadKey) + std::string(shim);
  if (!oldPreload.empty())
    preload += ":" + oldPreload;
  env.storage.push_back(std::move(preload));
  env.storage.push_back(fdKey + std::to_string(poolFd));

  // only now: growing storage moves short strings, which would leave earlier
  // pointers dangling
  env.pointers.reserve(env.storage.size() + 1);
  for (std::string &var : env.storage)
    env.pointers.push_back(var.data());
  env.pointers.push_back(nullptr);
  return env;
}

struct SpawnError {
  enum class Kind { Setup, Exec } kind;
  const char *step; // "pipe2", "fork", "fcntl", "execvpe"
  int error;
};

namespace detail {
enum ChildStage : int {
  StageFcntl = 1,
  StageExec = 2,
};

[[noreturn]] inline void childFail(int statusFd, int stage) noexcept {
  std::array<int, 2> report{stage, errno};
  [[maybe_unused]] auto written =
      ::write(statusFd, report.data(), sizeof(report));
  _exit(execFailedExit);
}

[[noreturn]] inline void becomeTarget(char *const *program, char *const *envp,
                                      int poolFd, int statusFd) noexcept {
  if (::fcntl(poolFd, F_SETFD, 0) == -1)
    childFail(statusFd, ChildStage::StageFcntl);

  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  for (const auto sig : forwardedSignals)
    ::sigaction(sig, &dfl, nullptr);

  sigset_t empty;
  ::sigemptyset(&empty);
  ::sigprocmask(SIG_SETMASK, &empty, nullptr);

  ::execvpe(program[0], program, envp);

  // only reached on failure with errno set
  childFail(statusFd, ChildStage::StageExec);
}

} // namespace detail

// blocks until pid has ende, and returns wait status.
inline int waitFor(pid_t pid) {
  int status = 0;
  while (::waitpid(pid, &status, 0) == -1 && errno == EINTR)
    ;
  return status;
}

// the program's code, or 128 + N for signal N.
inline int exitCodeFor(int status) {
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return launcherErrorExit;
}

inline std::expected<pid_t, SpawnError>
spawnTarget(char *const *program, char *const *envp, int poolFd) {
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) == -1)
    return std::unexpected(SpawnError{SpawnError::Kind::Setup, "pipe2", errno});

  // children gets a copy of every stdio buffer
  std::fflush(nullptr);

  pid_t pid = ::fork();
  if (pid == -1) {
    int error = errno; // set before close overwrites
    ::close(fds[0]);
    ::close(fds[1]);
    return std::unexpected(SpawnError{SpawnError::Kind::Setup, "fork", error});
  }

  if (pid == 0) {
    ::close(fds[0]);
    detail::becomeTarget(program, envp, poolFd, fds[1]);
  }

  ::close(fds[1]);

  std::array<int, 2> report{};
  auto *bytes = reinterpret_cast<char *>(report.data());
  std::size_t got = 0;
  while (got < sizeof(report)) {
    ssize_t n = ::read(fds[0], bytes + got, sizeof(report) - got);
    if (n == -1 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    got += static_cast<std::size_t>(n);
  }
  ::close(fds[0]);

  if (got == 0)
    return pid;

  waitFor(pid);
  if (got != sizeof(report))
    return std::unexpected(SpawnError{SpawnError::Kind::Exec, "execvpe", EIO});
  const char *step =
      report[0] == detail::ChildStage::StageFcntl ? "fcntl" : "execvpe";
  return std::unexpected(SpawnError{SpawnError::Kind::Exec, step, report[1]});
}

} // namespace tatrace::launcher
