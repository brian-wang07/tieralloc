// tieralloc-trace: launches a program with the shim preloaded and attached
// to a fresh pool, then watches it.

#include "launcher/options.hpp"
#include "launcher/signals.hpp"
#include "launcher/spawn.hpp"
#include "launcher/watch.hpp"
#include "tatrace/pool.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace tatrace::launcher;
using namespace tatrace;

int usageError(const std::string &message) {
  std::fprintf(stderr, "tieralloc-trace: %s\n%s", message.c_str(),
               usageText().c_str());
  return usageExit;
}

int runtimeError(const char *step, int error, int code) {
  std::fprintf(stderr, "tieralloc-trace: %s: %s\n", step, std::strerror(error));
  return code;
}

} // namespace

int main(int argc, char **argv) {
  if (wantsHelp(argc, argv)) {
    std::fputs(usageText().c_str(), stdout);
    return 0;
  }
  auto options = parseRunArgs(argc, argv, TA_SHIM_PATH);
  if (!options)
    return usageError(options.error());
  auto shim = resolveShim(options->shim);
  if (!shim)
    return usageError(shim.error());

  auto created = createPool(options->pool);
  if (!created)
    return runtimeError(created.error().step, created.error().error,
                        launcherErrorExit);

  auto signals = ForwardedSignals::create();
  if (!signals) {
    runtimeError(signals.error().step, signals.error().error,
                 launcherErrorExit);
    destroyPool(*created);
    return launcherErrorExit;
  }

  Environment env = buildEnvironment(environ, *shim, created->fd);

  auto pid = spawnTarget(options->program, env.envp(), created->fd);
  if (!pid) {
    SpawnError e = pid.error();
    int code;
    if (e.kind == SpawnError::Kind::Exec &&
        std::string_view(e.step) == "execvpe") {
      std::fprintf(stderr, "tieralloc-trace: cannot run %s: %s\n",
                   options->program[0], std::strerror(e.error));
      code = execFailedExit;
    } else {
      std::fprintf(stderr, "tieralloc-trace: %s: %s\n", e.step,
                   std::strerror(e.error));
      code =
          e.kind == SpawnError::Kind::Exec ? execFailedExit : launcherErrorExit;
    }
    destroyPool(*created);
    return code;
  }

  ::close(created->fd);
  created->fd = -1;

  auto watched = watchTarget(created->pool, *pid, signals->fd());
  if (!watched) {
    runtimeError(watched.error().step, watched.error().error,
                 launcherErrorExit);
    ::kill(*pid, SIGKILL);
    waitFor(*pid);
    destroyPool(*created);
    return launcherErrorExit;
  }

  int status = *watched;

  if (attachedPid(created->pool) != static_cast<std::uint32_t>(*pid))
    std::fprintf(stderr,
                 "tieralloc-trace: %s never loaded the shim (statically "
                 "linked, setuid, or LD_PRELOAD cleared?)\n",
                 options->program[0]);
  if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    std::fprintf(stderr, "tieralloc-trace: %s killed by signal %d (%s)\n",
                 options->program[0], sig, strsignal(sig));
  }

  destroyPool(*created);
  return exitCodeFor(status);
}
