// Checks for the launcher's command line (src/launcher/options.hpp): the
// defaults, both option spellings, that everything after "--" belongs to the
// program, each range and number check (narrowing and overflow included),
// help detection, and resolveShim on the real shim and a missing one. Exits
// nonzero if any check fails.

#include "launcher/options.hpp"
#include "ta/testing.hpp"

#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace tatrace;
using namespace tatrace::launcher;
using namespace ta::test;

constexpr std::string_view defaultShim = "/default/shim.so";

// A writable argv with a trailing nullptr, as main gets one.
class Argv {
public:
  Argv(std::initializer_list<const char *> args)
      : Argv(std::vector<const char *>(args)) {}
  explicit Argv(const std::vector<const char *> &args) {
    storage.emplace_back("tieralloc-trace");
    for (const char *arg : args)
      storage.emplace_back(arg);
    for (std::string &s : storage)
      pointers.push_back(s.data());
    pointers.push_back(nullptr);
  }
  Argv(const Argv &) = delete;
  Argv &operator=(const Argv &) = delete;

  int argc() const { return static_cast<int>(storage.size()); }
  char **argv() { return pointers.data(); }
  auto parse() { return parseRunArgs(argc(), argv(), defaultShim); }

private:
  std::vector<std::string> storage;
  std::vector<char *> pointers;
};

// The program's argv matches want exactly, ending in nullptr.
bool programIs(char *const *program,
               std::initializer_list<std::string_view> want) {
  if (program == nullptr)
    return false;
  std::size_t i = 0;
  for (std::string_view arg : want)
    if (program[i] == nullptr || program[i++] != arg)
      return false;
  return program[i] == nullptr;
}

void checkGood() {
  Argv plain{"run", "--", "prog", "a", "b"};
  auto r = plain.parse();
  check(r && r->pool.ringCount == defaultRingCount &&
            r->pool.ringCapacity == defaultRingCapacity &&
            r->pool.heartbeatTimeoutNs == defaultHeartbeatTimeoutNs &&
            r->shim == defaultShim && programIs(r->program, {"prog", "a", "b"}),
        "parse", "run -- prog a b: defaults, and the program's argv");
  check(r && r->program == plain.argv() + 3, "parse",
        "the program list points into argv itself");

  Argv both{"run", "--rings", "8", "--ring-capacity=1024", "--", "prog"};
  r = both.parse();
  check(r && r->pool.ringCount == 8 && r->pool.ringCapacity == 1024, "parse",
        "--name value and --name=value both work");

  Argv timeout{"run", "--heartbeat-timeout-ms", "50", "--", "prog"};
  r = timeout.parse();
  check(r && r->pool.heartbeatTimeoutNs == 50'000'000, "parse",
        "--heartbeat-timeout-ms is converted to ns");

  Argv theirs{"run", "--", "prog", "--rings", "3", "--"};
  r = theirs.parse();
  check(r && r->pool.ringCount == defaultRingCount &&
            programIs(r->program, {"prog", "--rings", "3", "--"}),
        "parse", "arguments after -- are the program's, even ours");

  Argv shim{"run", "--shim", "/x.so", "--", "prog"};
  r = shim.parse();
  check(r && r->shim == "/x.so", "parse", "--shim is taken as given");

  Argv bounds{"run", "--rings=4096", "--ring-capacity", "64", "--", "p"};
  r = bounds.parse();
  check(r && r->pool.ringCount == maxRingCount && r->pool.ringCapacity == 64,
        "parse", "the largest ring count and smallest capacity are accepted");
}

struct Bad {
  const char *what;
  std::vector<const char *> args;
};

void checkBad() {
  const Bad cases[] = {
      {"no command", {}},
      {"unknown command", {"frob", "--", "p"}},
      {"run alone", {"run"}},
      {"no --", {"run", "--rings", "8"}},
      {"nothing after --", {"run", "--"}},
      {"a program without --", {"run", "prog"}},
      {"unknown option", {"run", "--bogus", "1", "--", "p"}},
      {"0 rings", {"run", "--rings", "0", "--", "p"}},
      {"too many rings", {"run", "--rings", "4097", "--", "p"}},
      {"rings past 32 bits", {"run", "--rings", "4294967297", "--", "p"}},
      {"capacity not a power of two",
       {"run", "--ring-capacity", "1000", "--", "p"}},
      {"capacity too small", {"run", "--ring-capacity", "32", "--", "p"}},
      {"capacity too large", {"run", "--ring-capacity", "33554432", "--", "p"}},
      {"zero timeout", {"run", "--heartbeat-timeout-ms", "0", "--", "p"}},
      {"timeout overflow",
       {"run", "--heartbeat-timeout-ms", "99999999999999999", "--", "p"}},
      {"value missing before --", {"run", "--rings", "--", "p"}},
      {"value missing at the end", {"run", "--rings"}},
      {"trailing junk", {"run", "--rings", "8x", "--", "p"}},
      {"a sign", {"run", "--rings", "-1", "--", "p"}},
      {"leading space", {"run", "--rings", " 8", "--", "p"}},
      {"empty =value", {"run", "--rings=", "--", "p"}},
      {"empty shim", {"run", "--shim=", "--", "p"}},
      {"a plus sign", {"run", "--rings", "+8", "--", "p"}},
      {"a short option", {"run", "-o", "x", "--", "p"}},
  };
  std::size_t rejected = 0;
  for (const Bad &c : cases) {
    Argv argv(c.args);
    auto r = argv.parse();
    if (!r && !r.error().empty())
      ++rejected;
    else
      check(false, "reject", "%s was accepted", c.what);
  }
  check(rejected == std::size(cases), "reject",
        "%zu of %zu malformed command lines give an error message", rejected,
        std::size(cases));

  Argv narrow{"run", "--rings", "4294967297", "--", "p"};
  auto r = narrow.parse();
  check(!r && r.error().find("4294967297") != std::string::npos, "reject",
        "the message quotes the bad value: %s",
        r ? "(accepted)" : r.error().c_str());
}

void checkHelp() {
  Argv h{"-h"}, help{"--help"}, runHelp{"run", "--help"}, run{"run", "--", "p"},
      theirs{"run", "--", "p", "--help"};
  check(wantsHelp(h.argc(), h.argv()) && wantsHelp(help.argc(), help.argv()) &&
            wantsHelp(runHelp.argc(), runHelp.argv()),
        "help", "-h, --help and run --help ask for help");
  check(!wantsHelp(run.argc(), run.argv()) &&
            !wantsHelp(theirs.argc(), theirs.argv()),
        "help", "a normal run, or --help meant for the program, doesn't");
  std::string text = usageText();
  check(text.find("default 64") != std::string::npos &&
            text.find("default 65536") != std::string::npos &&
            text.find("default 2000") != std::string::npos,
        "help", "the usage shows the real defaults");
}

void checkResolveShim() {
  auto real = resolveShim(TA_SHIM_PATH);
  check(real && real->starts_with('/') && real->ends_with(".so"), "shim",
        "the build's shim resolves to an absolute path: %s",
        real ? real->c_str() : real.error().c_str());
  auto missing = resolveShim("/nonexistent/libtieralloc-trace.so");
  check(!missing &&
            missing.error().find(std::strerror(ENOENT)) != std::string::npos,
        "shim", "a missing shim is an error naming ENOENT: %s",
        missing ? "(resolved)" : missing.error().c_str());
}

} // namespace

int main() {
  checkGood();
  checkBad();
  checkHelp();
  checkResolveShim();
  return finish();
}
