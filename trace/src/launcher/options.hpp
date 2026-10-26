#pragma once

// tieralloc-trace command line
// tieralloc-trace run [options] -- program [args...]

#include "tatrace/pool.hpp"

#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <unistd.h>

namespace tatrace::launcher {

struct RunOptions {
  PoolConfig pool;
  std::string_view shim;
  char *const *program = nullptr;
};

// 1 is the launcher's own runtime errors; the program's status passes
// through otherwise (see usageText).
inline constexpr int usageExit = 2;

inline constexpr std::uint64_t nsPerMs = 1'000'000;
inline constexpr std::uint64_t maxTimeoutMs =
    std::numeric_limits<std::uint64_t>::max() / nsPerMs;
// the smallest capacity validRingShape accepts
inline constexpr std::uint64_t minCapacity = std::bit_ceil(minRingCapacity);

// built from the constants, so the defaults shown can't drift from the real
// ones.
inline std::string usageText() {
  return std::format(
      "usage: tieralloc-trace run [options] -- program [args...]\n"
      "  --rings N                 rings in the pool, one per live thread "
      "(default {})\n"
      "  --ring-capacity N         slots per ring, a power of two "
      "(default {})\n"
      "  --heartbeat-timeout-ms N  producers give up after this much watcher "
      "silence (default {})\n"
      "  --shim PATH               the preload shim (default: this build's)\n"
      "exit status: the program's, 128+N if it died of signal N, 127 if it\n"
      "  couldn't be run, 1 or 2 for tieralloc-trace's own errors\n",
      defaultRingCount, defaultRingCapacity,
      defaultHeartbeatTimeoutNs / nsPerMs);
}

// -h or --help, as the command or right after run.
inline bool wantsHelp(int argc, char **argv) {
  auto isHelp = [](std::string_view arg) {
    return arg == "-h" || arg == "--help";
  };
  if (argc >= 2 && isHelp(argv[1]))
    return true;
  return argc >= 3 && std::string_view(argv[1]) == "run" && isHelp(argv[2]);
}

namespace detail {

// a whole decimal number: no sign, no spaces, nothing after it.
inline std::optional<std::uint64_t> parseCount(std::string_view text) {
  if (text.empty() || text.front() < '0' || text.front() > '9')
    return std::nullopt;
  std::uint64_t value = 0;
  const char *end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{} || ptr != end)
    return std::nullopt;
  return value;
}

// stores one option's value; the error message on a bad one.
inline std::optional<std::string> applyOption(RunOptions &options,
                                              std::string_view name,
                                              std::string_view value) {
  std::optional<std::uint64_t> n = parseCount(value);
  if (name == "--rings") {
    // checked as 64 bits before narrowing, so 2^32 + 1 can't become 1
    if (!n || *n < 1 || *n > maxRingCount)
      return std::format("--rings: '{}' must be a whole number from 1 to {}",
                         value, maxRingCount);
    options.pool.ringCount = static_cast<std::uint32_t>(*n);
  } else if (name == "--ring-capacity") {
    if (!n || !std::has_single_bit(*n) || *n < minCapacity ||
        *n > maxRingCapacity)
      return std::format(
          "--ring-capacity: '{}' must be a power of two from {} to {}", value,
          minCapacity, maxRingCapacity);
    options.pool.ringCapacity = *n;
  } else if (name == "--heartbeat-timeout-ms") {
    if (!n || *n < 1 || *n > maxTimeoutMs)
      return std::format(
          "--heartbeat-timeout-ms: '{}' must be a whole number from 1 to {}",
          value, maxTimeoutMs);
    options.pool.heartbeatTimeoutNs = *n * nsPerMs;
  } else if (name == "--shim") {
    if (value.empty())
      return std::string("--shim needs a path");
    options.shim = value;
  } else {
    return std::format("unknown option '{}'", name);
  }
  return std::nullopt;
}

} // namespace detail

// up to the first "--"
inline std::expected<RunOptions, std::string>
parseRunArgs(int argc, char **argv, std::string_view defaultShim) {
  if (argc < 2)
    return std::unexpected(std::string("no command given"));
  if (std::string_view(argv[1]) != "run")
    return std::unexpected(std::format("unknown command '{}'", argv[1]));

  RunOptions options{.pool = {}, .shim = defaultShim};
  for (int i = 2; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--") {
      if (i + 1 == argc)
        return std::unexpected(std::string("no program given after --"));
      options.program = argv + i + 1;
      break;
    }
    // a bare word before "--" is most likely a forgotten "--"; reading it as
    // the program would hide that
    if (!arg.starts_with("--"))
      return std::unexpected(
          std::format("unexpected argument '{}' before --", arg));

    // --name=value or --name value
    std::string_view name = arg, value;
    if (auto eq = arg.find('='); eq != std::string_view::npos) {
      name = arg.substr(0, eq);
      value = arg.substr(eq + 1);
    } else if (i + 1 < argc && std::string_view(argv[i + 1]) != "--") {
      value = argv[++i];
    } else {
      return std::unexpected(std::format("{} needs a value", name));
    }
    if (auto error = detail::applyOption(options, name, value))
      return std::unexpected(std::move(*error));
  }

  if (options.program == nullptr)
    return std::unexpected(std::string("missing -- before the program"));
  // the per-option ranges already guarantee this; kept as the one rule
  // createPool will hold the config to
  if (!validRingShape(options.pool.ringCount, options.pool.ringCapacity))
    return std::unexpected(std::string("invalid ring shape"));
  return options;
}

inline std::expected<std::string, std::string>
resolveShim(std::string_view path) {
  // realpath needs a terminated string
  std::string given(path);
  std::unique_ptr<char, decltype(&std::free)> resolved(
      ::realpath(given.c_str(), nullptr), &std::free);
  if (!resolved || ::access(resolved.get(), R_OK) != 0) {
    int error = errno;
    return std::unexpected(
        std::format("shim {}: {}", path, std::strerror(error)));
  }
  return std::string(resolved.get());
}

} // namespace tatrace::launcher
