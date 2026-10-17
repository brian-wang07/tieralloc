#pragma once

#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tabench {

class ArgError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

enum class ParseError { Empty, NotANumber, OutOfRange, TrailingCharacters };

inline const char *describe(ParseError error) noexcept {
  switch (error) {
  case ParseError::Empty:
    return "is empty";
  case ParseError::NotANumber:
    return "is not a number";
  case ParseError::OutOfRange:
    return "is out of range";
  case ParseError::TrailingCharacters:
    return "has trailing characters";
  }
  return "is invalid";
}

inline std::expected<std::uint64_t, ParseError>
parseU64(std::string_view text) noexcept {
  if (text.empty())
    return std::unexpected(ParseError::Empty);
  std::uint64_t value{};
  const char *end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec == std::errc::invalid_argument)
    return std::unexpected(ParseError::NotANumber);
  if (ec == std::errc::result_out_of_range)
    return std::unexpected(ParseError::OutOfRange);
  if (ptr != end)
    return std::unexpected(ParseError::TrailingCharacters);
  return value;
}

inline std::expected<double, ParseError> parseDouble(std::string_view text) {
  if (text.empty())
    return std::unexpected(ParseError::Empty);
  if (std::isspace(static_cast<unsigned char>(text.front())))
    return std::unexpected(ParseError::NotANumber);
  std::string buffer(text); // strtod needs a null-terminated string.
  char *end = nullptr;
  errno = 0;
  double value = std::strtod(buffer.c_str(), &end);
  if (end == buffer.c_str())
    return std::unexpected(ParseError::NotANumber);
  if (end != buffer.c_str() + buffer.size())
    return std::unexpected(ParseError::TrailingCharacters);
  if (std::isnan(value))
    return std::unexpected(ParseError::NotANumber);
  if (std::isinf(value) || (errno == ERANGE && std::abs(value) == HUGE_VAL))
    return std::unexpected(ParseError::OutOfRange);
  return value;
}

class Args {
public:
  // argv[0] is the program name and is skipped.
  Args(int argc, const char *const *argv) {
    for (int i = 1; i < argc; ++i) {
      Arg arg = parseArgument(argv[i]);
      if (find(arg.key))
        fail("--{} given more than once", arg.key);
      args.push_back(arg);
    }
  }

  std::uint64_t getU64(std::string_view key, std::uint64_t defaultValue) {
    return getParsed(key, defaultValue, parseU64);
  }

  double getDouble(std::string_view key, double defaultValue) {
    return getParsed(key, defaultValue, parseDouble);
  }

  // The returned view points into argv, or is defaultValue itself.
  std::string_view getString(std::string_view key,
                             std::string_view defaultValue) {
    std::string_view value = defaultValue;
    if (Arg *arg = take(key))
      value = requireValue(*arg);
    record(key, std::string(value));
    return value;
  }

  // True if --key was given. A flag takes no value.
  bool getFlag(std::string_view key) {
    bool value = false;
    if (Arg *arg = take(key)) {
      if (arg->hasValue)
        fail("--{} is a flag and takes no value", key);
      value = true;
    }
    record(key, value);
    return value;
  }

  // Arguments no getter has asked for, in command-line order.
  std::vector<std::string_view> unusedKeys() const {
    std::vector<std::string_view> unused;
    for (const Arg &arg : args)
      if (!arg.used)
        unused.push_back(arg.key);
    return unused;
  }

  // Call after reading every parameter, so a typo such as --sede=1 fails
  // instead of silently running with the default.
  void checkAllUsed() const {
    std::vector<std::string_view> unused = unusedKeys();
    if (unused.empty())
      return;
    fail("unknown argument{} --{} (known: {})", unused.size() > 1 ? "s" : "",
         join(unused, ", --"), knownKeys());
  }

  // Every value read so far, including defaults, in the order it was read.
  const nlohmann::ordered_json &params() const noexcept { return recorded; }

private:
  struct Arg {
    std::string_view key, value;
    bool hasValue;
    bool used;
  };

  std::vector<Arg> args;
  nlohmann::ordered_json recorded = nlohmann::ordered_json::object();

  // Throws ArgError with a std::format message. The format string is checked
  // against the arguments at compile time.
  template <class... FmtArgs>
  [[noreturn]] static void fail(std::format_string<FmtArgs...> fmt,
                                FmtArgs &&...fmtArgs) {
    throw ArgError(std::format(fmt, std::forward<FmtArgs>(fmtArgs)...));
  }

  static Arg parseArgument(std::string_view text) {
    if (!text.starts_with("--"))
      fail("expected --key or --key=value, got '{}'", text);
    std::string_view body = text.substr(2);
    std::size_t equals = body.find('=');
    std::string_view key = body.substr(0, equals);
    if (key.empty())
      fail("missing key in '{}'", text);
    if (equals == std::string_view::npos)
      return {key, {}, false, false};
    return {key, body.substr(equals + 1), true, false};
  }

  Arg *find(std::string_view key) {
    for (Arg &arg : args)
      if (arg.key == key)
        return &arg;
    return nullptr;
  }

  // Finds key and marks it used; nullptr if it was not given.
  Arg *take(std::string_view key) {
    Arg *arg = find(key);
    if (arg)
      arg->used = true;
    return arg;
  }

  static std::string_view requireValue(const Arg &arg) {
    if (!arg.hasValue)
      fail("--{} needs a value (--{}=...)", arg.key, arg.key);
    return arg.value;
  }

  template <class T, class Parse>
  T getParsed(std::string_view key, T defaultValue, Parse parse) {
    T value = defaultValue;
    if (Arg *arg = take(key)) {
      std::string_view text = requireValue(*arg);
      auto parsed = parse(text);
      if (!parsed)
        fail("--{}: '{}' {}", key, text, describe(parsed.error()));
      value = *parsed;
    }
    record(key, value);
    return value;
  }

  template <class T> void record(std::string_view key, T value) {
    recorded[std::string(key)] = std::move(value);
  }

  std::string knownKeys() const {
    std::string out;
    for (const auto &item : recorded.items())
      out += (out.empty() ? "--" : ", --") + item.key();
    return out.empty() ? "none" : out;
  }

  static std::string join(const std::vector<std::string_view> &parts,
                          std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i)
        out += separator;
      out += parts[i];
    }
    return out;
  }
};

} // namespace tabench
