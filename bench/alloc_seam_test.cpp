// Seam test: checks bench/alloc/seam.hpp against the backend this target was
// built with (TA_BACKEND_*). CMake builds it with contracts enforced and seam
// checks on; checks that need those report SKIP when they are off (Clang's
// fallback macros, or a different semantic).
//
// --expect-allocator=NAME fails unless that allocator serves malloc. ctest runs
// the plain build once per allocator built by scripts/build-allocators.sh, so
// every check also runs under jemalloc, mimalloc and tcmalloc.

#include "alloc/seam.hpp"
#include "harness/allocator.hpp"
#include "harness/args.hpp"
#include "harness/report.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace alloc = tabench::alloc;
using alloc::Role;
using alloc::seamStats;

// A type that declares its role, to check that make() and containers use it.
struct HotNode {
  std::uint64_t key;
  std::uint64_t value;
};
template <> inline constexpr Role tabench::alloc::roleOf<HotNode> = Role::Hot;

namespace {

int failures = 0;

[[gnu::format(printf, 3, 4)]] void check(bool ok, const char *name,
                                         const char *fmt, ...) {
  std::printf("%s  %-9s ", ok ? "PASS" : "FAIL", name);
  va_list args;
  va_start(args, fmt);
  std::vprintf(fmt, args);
  va_end(args);
  std::printf("\n");
  if (!ok)
    ++failures;
}

void skip(const char *name, const char *why) {
  std::printf("SKIP  %-9s %s\n", name, why);
}

using RoleBytes = std::array<std::uint64_t, alloc::roleCount>;

// How far one role's live bytes moved since before, and whether every other
// role stayed put. Each check compares against a snapshot of its own, so
// checks don't depend on each other.
std::uint64_t grew(const RoleBytes &before, Role role) {
  return seamStats.liveBytes[alloc::index(role)] - before[alloc::index(role)];
}

bool othersUnchanged(const RoleBytes &before, Role role) {
  for (std::size_t i = 0; i < alloc::roleCount; ++i)
    if (i != alloc::index(role) && seamStats.liveBytes[i] != before[i])
      return false;
  return true;
}

// Contract checks only abort under these semantics; Clang has no contracts
// at all, so its preconditions and postconditions vanish.
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

void checkAlignment() {
  int count = 0;
  bool aligned = true, counted = true, intact = true;
  for (std::size_t i = 0; i < alloc::roleCount; ++i) {
    alloc::withRole(static_cast<Role>(i), [&](auto r) {
      constexpr Role R = decltype(r)::value;
      for (std::size_t align = 1; align <= 4096; align *= 2) {
        // Sizes are multiples of the alignment, as the seam requires.
        for (std::size_t bytes :
             {std::size_t{0}, align, 3 * align, 8 * align}) {
          RoleBytes before = seamStats.liveBytes;
          void *p = alloc::allocate<R>(bytes, align);
          ++count;
          aligned &= reinterpret_cast<std::uintptr_t>(p) % align == 0;
          counted &= grew(before, R) == bytes && othersUnchanged(before, R);
          auto *bytesOf = static_cast<unsigned char *>(p);
          std::memset(p, 0xa5, bytes);
          intact &= std::all_of(bytesOf, bytesOf + bytes,
                                [](unsigned char c) { return c == 0xa5; });
          alloc::deallocate<R>(p, bytes, align);
          counted &= seamStats.liveBytes == before;
        }
      }
    });
  }
  check(aligned, "align", "%d allocations aligned (every role, 1..4096)",
        count);
  check(counted, "align", "live bytes rise by the request, in its role only");
  check(intact, "align", "every block writable over its full size");
}

void checkZeroBytes() {
  bool ok = true;
  for (std::size_t i = 0; i < alloc::roleCount; ++i) {
    alloc::withRole(static_cast<Role>(i), [&](auto r) {
      constexpr Role R = decltype(r)::value;
      void *a = alloc::allocate<R>(0, 1);
      void *b = alloc::allocate<R>(0, 1);
      ok &= a != nullptr && b != nullptr && a != b;
      alloc::deallocate<R>(a, 0, 1);
      alloc::deallocate<R>(b, 0, 1);
    });
  }
  check(ok, "zero", "0-byte requests return distinct non-null pointers");
}

// Allocators are empty and rebinding keeps the container's role, so node and
// bucket allocations land in it too.
static_assert(sizeof(alloc::Vector<int>) == sizeof(std::vector<int>));
static_assert(sizeof(alloc::Map<int, int>) == sizeof(std::map<int, int>));
static_assert(sizeof(alloc::UnorderedMap<int, int>) ==
              sizeof(std::unordered_map<int, int>));
static_assert(
    std::allocator_traits<alloc::Map<int, int, Role::Cold>::allocator_type>::
        rebind_alloc<double>::role == Role::Cold);
static_assert(alloc::Vector<HotNode>::allocator_type::role == Role::Hot);

template <class Build>
void checkContainer(const char *what, Role role, Build build) {
  RoleBytes before = seamStats.liveBytes;
  bool rose, onlyItsRole;
  {
    auto container = build();
    rose = grew(before, role) > 0;
    onlyItsRole = othersUnchanged(before, role);
  }
  bool returned = seamStats.liveBytes == before;
  check(rose && onlyItsRole && returned, "container",
        "%s: %s bytes only, all returned on destruction", what,
        alloc::toString(role));
}

void checkContainers() {
  checkContainer("Vector", Role::Hot,
                 [] { return alloc::Vector<int, Role::Hot>(1000, 1); });
  checkContainer("List", Role::Cold,
                 [] { return alloc::List<int, Role::Cold>{1, 2, 3}; });
  checkContainer("Deque", Role::Transient,
                 [] { return alloc::Deque<int, Role::Transient>(1000, 1); });
  checkContainer("Map", Role::Hot, [] {
    alloc::Map<int, int, Role::Hot> m;
    for (int i = 0; i < 100; ++i)
      m[i] = i;
    return m;
  });
  checkContainer("UnorderedMap", Role::Cold, [] {
    alloc::UnorderedMap<int, int, Role::Cold> m;
    for (int i = 0; i < 100; ++i)
      m[i] = i;
    return m;
  });
  checkContainer("String", Role::Hot, [] {
    // Longer than the small-string buffer, so it allocates.
    return alloc::String<Role::Hot>(100, 'x');
  });
  checkContainer("Vector<HotNode> (roleOf)", Role::Hot,
                 [] { return alloc::Vector<HotNode>(10); });
}

void checkStats() {
  alloc::SeamRegion seam;
  seam.begin = seamStats.beginRegion();
  auto *cold = alloc::make<std::uint64_t, Role::Cold>(std::uint64_t{1});
  void *transient = alloc::allocate<Role::Transient>(256, 16);
  alloc::deallocate<Role::Transient>(transient, 256, 16);
  alloc::destroy<std::uint64_t, Role::Cold>(cold);
  seam.end = seamStats;

  auto count = [&](const auto &field, Role role) {
    return (seam.end.*field)[alloc::index(role)] -
           (seam.begin.*field)[alloc::index(role)];
  };
  using alloc::SeamStats;
  bool counts = count(&SeamStats::allocations, Role::Cold) == 1 &&
                count(&SeamStats::allocations, Role::Transient) == 1 &&
                count(&SeamStats::frees, Role::Cold) == 1 &&
                count(&SeamStats::frees, Role::Transient) == 1 &&
                count(&SeamStats::allocations, Role::Hot) == 0;
  check(counts, "stats", "allocation and free counts per role");
  // Both blocks were live at once: 8 + 256 bytes above the region's start.
  std::uint64_t peak = seam.end.peakLiveBytes - seam.begin.totalLiveBytes;
  check(peak == 264, "stats", "peak measured from the region start: %llu B",
        static_cast<unsigned long long>(peak));

  nlohmann::ordered_json j = alloc::seamJson(seam, 0);
  std::vector<std::string> keys;
  for (const auto &item : j.items())
    keys.push_back(item.key());
  std::vector<std::string> expected{"backend",
                                    "checks",
                                    "allocations",
                                    "frees",
                                    "liveBytesBegin",
                                    "liveBytesEnd",
                                    "totalLiveBytesBegin",
                                    "totalLiveBytesEnd",
                                    "peakLiveBytes",
                                    "leakedBytes",
                                    "backendStats"};
  check(keys == expected && j["allocations"]["cold"] == 1 &&
            j["backend"] == std::string(alloc::Backend::name),
        "json", "seamJson has every field, in order");
}

void checkRandomized() {
  struct Block {
    void *p;
    std::size_t bytes, align;
    Role role;
    unsigned char tag;
  };
  RoleBytes before = seamStats.liveBytes;
  std::mt19937_64 rng(0x5eed);
  // std::vector, not the seam's: the bookkeeping must not count itself.
  std::vector<Block> blocks;
  std::size_t corrupt = 0, operations = 0;

  auto release = [&](std::size_t i) {
    const Block &b = blocks[i];
    auto *bytes = static_cast<unsigned char *>(b.p);
    if (!std::all_of(bytes, bytes + b.bytes,
                     [&](unsigned char c) { return c == b.tag; }))
      ++corrupt;
    alloc::withRole(b.role, [&](auto r) {
      alloc::deallocate<decltype(r)::value>(b.p, b.bytes, b.align);
    });
    blocks[i] = blocks.back();
    blocks.pop_back();
  };

  for (; operations < 50'000; ++operations) {
    if (blocks.size() < 2000 && (blocks.empty() || rng() % 2 == 0)) {
      Block b;
      b.align = std::size_t{1} << (rng() % 8);
      b.bytes = b.align * (rng() % 64);
      b.role = static_cast<Role>(rng() % alloc::roleCount);
      b.tag = static_cast<unsigned char>(rng());
      b.p = alloc::withRole(b.role, [&](auto r) {
        return alloc::allocate<decltype(r)::value>(b.bytes, b.align);
      });
      std::memset(b.p, b.tag, b.bytes);
      blocks.push_back(b);
    } else {
      release(rng() % blocks.size());
    }
  }
  while (!blocks.empty())
    release(blocks.size() - 1);
  check(corrupt == 0, "random",
        "%zu operations, no block overwritten by another", operations);
  check(seamStats.liveBytes == before, "random", "live bytes back to start");
}

struct Throws {
  explicit Throws(bool fail) {
    if (fail)
      throw std::runtime_error("constructor failed");
  }
  std::uint64_t pad[4]{};
};

void checkThrowingConstructor() {
  RoleBytes before = seamStats.liveBytes;
  std::uint64_t allocations = seamStats.allocations[alloc::index(Role::Hot)];
  std::uint64_t frees = seamStats.frees[alloc::index(Role::Hot)];
  bool caught = false;
  try {
    (void)alloc::make<Throws, Role::Hot>(true);
  } catch (const std::runtime_error &) {
    caught = true;
  }
  check(caught && seamStats.liveBytes == before &&
            seamStats.allocations[alloc::index(Role::Hot)] == allocations + 1 &&
            seamStats.frees[alloc::index(Role::Hot)] == frees + 1,
        "throw", "a throwing constructor frees its storage and rethrows");

  Throws *ok = alloc::make<Throws, Role::Hot>(false);
  check(grew(before, Role::Hot) == sizeof(Throws), "throw",
        "the same type builds normally when its constructor succeeds");
  alloc::destroy<Throws, Role::Hot>(ok);
}

void checkRuntimeRoles() {
  bool agree = true;
  for (std::size_t i = 0; i < alloc::roleCount; ++i) {
    Role role = static_cast<Role>(i);
    RoleBytes before = seamStats.liveBytes;
    std::uint64_t *p = alloc::makeAs<std::uint64_t>(role, std::uint64_t{i});
    agree &= *p == i && grew(before, role) == sizeof(std::uint64_t) &&
             othersUnchanged(before, role);
    alloc::destroyAs(p, role);
    agree &= seamStats.liveBytes == before;
  }
  check(agree, "makeAs", "makeAs/destroyAs use the role passed at run time");

  RoleBytes before = seamStats.liveBytes;
  HotNode *node = alloc::make<HotNode>(HotNode{1, 2});
  bool viaRoleOf = grew(before, Role::Hot) == sizeof(HotNode);
  alloc::destroy(node);
  check(viaRoleOf && seamStats.liveBytes == before, "roleOf",
        "make<T>() takes the role declared by roleOf<T>");

  const HotNode *constant = alloc::make<const HotNode>(HotNode{3, 4});
  bool constOk = constant->value == 4 && grew(before, Role::Hot) > 0;
  alloc::destroy(constant);
  alloc::destroy(static_cast<HotNode *>(nullptr));
  check(constOk && seamStats.liveBytes == before, "const",
        "make/destroy of a const T, and destroy(nullptr) is a no-op");
}

void checkInterface() {
  static_assert(alloc::SeamBackend<alloc::Backend>);
  check(std::string_view(alloc::Backend::name) == tabench::backendName(),
        "interface", "seam backend '%s' matches the record's backend",
        std::string(alloc::Backend::name).c_str());
}

// A backend that breaks the seam's postcondition, to check it is enforced.
struct MisalignedBackend {
  static constexpr std::string_view name = "misaligned";
  alignas(64) static inline unsigned char buffer[128];
  template <Role R> static void *allocate(std::size_t, std::size_t) {
    return buffer + 8;
  }
  template <Role R>
  static void deallocate(void *, std::size_t, std::size_t) noexcept {}
  static void endPhase() noexcept {}
  static nlohmann::ordered_json extraStats() { return {}; }
};
static_assert(alloc::SeamBackend<MisalignedBackend>);

#if defined(__linux__) || defined(__APPLE__)
// Runs f in a forked child with stderr silenced (the violation reports would
// drown the output) and core dumps off (systemd-coredump would otherwise store
// one per abort, which takes seconds under mimalloc's large reservations). True
// if the child died of SIGABRT, which is how both enforce and quick_enforce end
// on GCC 16, and how the fallbacks end.
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

template <class F>
void checkAborts(bool enabled, const char *what, const char *why, F f) {
  if (!enabled)
    skip("abort", why);
  else
    check(abortsInChild(f), "abort", "%s", what);
}

void checkContracts() {
  const char *noContracts = "contracts not enforced in this build";
  bool pre = contractsAbort();
  checkAborts(pre, "precondition: size not a multiple of the alignment",
              noContracts, [] { (void)alloc::allocate<Role::Hot>(8, 16); });
  checkAborts(pre, "precondition: alignment not a power of two", noContracts,
              [] { (void)alloc::allocate<Role::Hot>(24, 3); });
  checkAborts(pre, "precondition: free of a null pointer", noContracts,
              [] { alloc::deallocate<Role::Hot>(nullptr, 8, 8); });
  checkAborts(pre, "precondition: free with the wrong role (live underflow)",
              noContracts, [] {
                void *p = alloc::allocate<Role::Hot>(64, 16);
                alloc::deallocate<Role::Cold>(p, 64, 16);
              });
  checkAborts(
      pre, "postcondition: backend returns a misaligned block", noContracts,
      [] { (void)alloc::allocate<Role::Hot, MisalignedBackend>(16, 16); });
  checkAborts(assertsAbort(), "assertion: makeAs with an out-of-range role",
              "assertions not checked in this build",
              [] { (void)alloc::makeAs<int>(static_cast<Role>(7), 1); });
  checkAborts(
      TA_SEAM_CHECKS != 0, "seam check: allocation from a second thread",
      "TA_SEAM_CHECKS is off", [] {
        std::thread([] { (void)alloc::allocate<Role::Hot>(16, 16); }).join();
      });
}
#else
void checkContracts() { skip("abort", "fork() needed (Linux and macOS)"); }
#endif

} // namespace

int main(int argc, char **argv) {
  tabench::Args args(argc, argv);
  std::string_view expectedAllocator = args.getString("expect-allocator", "");
  args.checkAllUsed();

  tabench::AllocatorInfo allocator = tabench::detectAllocator();
  std::printf("INFO  seam      backend=%s allocator=%s %s contracts=%s "
              "checks=%d\n",
              std::string(alloc::Backend::name).c_str(), allocator.name.c_str(),
              allocator.version.c_str(), TA_CONTRACT_SEMANTIC, TA_SEAM_CHECKS);
  if (!expectedAllocator.empty())
    check(allocator.name == expectedAllocator, "allocator",
          "malloc served by %s (expected %.*s)", allocator.name.c_str(),
          static_cast<int>(expectedAllocator.size()), expectedAllocator.data());

  checkInterface();
  checkAlignment();
  checkZeroBytes();
  checkContainers();
  checkStats();
  checkRandomized();
  checkThrowingConstructor();
  checkRuntimeRoles();
  checkContracts();
  check(seamStats.totalLiveBytes == 0, "leaks",
        "no seam bytes live at exit (%llu)",
        static_cast<unsigned long long>(seamStats.totalLiveBytes));

  if (failures == 0)
    std::printf("OK: 0 check(s) failed\n");
  else
    std::printf("FAILED: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
