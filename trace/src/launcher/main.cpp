// tieralloc-trace: launches a program with the shim preloaded and attached
// to a fresh pool, then watches it. For now it only parses and validates its
// command line and prints what it would launch.

#include "launcher/options.hpp"

#include <cstdio>
#include <string>

namespace {

using namespace tatrace::launcher;

int usageError(const std::string &message) {
  std::fprintf(stderr, "tieralloc-trace: %s\n%s", message.c_str(),
               usageText().c_str());
  return usageExit;
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

  // Temporary: step 2 replaces this with the launch.
  std::printf("rings %u, capacity %llu, timeout %llu ms\n",
              options->pool.ringCount,
              static_cast<unsigned long long>(options->pool.ringCapacity),
              static_cast<unsigned long long>(options->pool.heartbeatTimeoutNs /
                                              nsPerMs));
  std::printf("shim %s\nprogram", shim->c_str());
  for (char *const *arg = options->program; *arg != nullptr; ++arg)
    std::printf(" %s", *arg);
  std::printf("\n");
  return 0;
}
