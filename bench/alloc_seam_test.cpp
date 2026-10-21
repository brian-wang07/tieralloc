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
#include "ta/testing.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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

using namespace ta::test;

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

#if defined(TA_BACKEND_POOLS)
// Pools-only checks. Earlier checks have already used the pools, so every
// check here compares against a snapshot or picks state it can rely on, never
// absolute counts.
namespace pools = tabench::alloc::pools;

// A class with an empty free list, so its next allocations are bumped.
std::uint8_t classWithoutFreeSlots() {
  for (std::size_t i = pools::hotClassCount; i-- > 0;)
    if (pools::hotClasses.classes[i].freeList == nullptr)
      return static_cast<std::uint8_t>(i);
  return 0;
}

std::uintptr_t pageOf(const void *p) {
  return reinterpret_cast<std::uintptr_t>(p) & ~(pools::pageBytes - 1);
}

void checkHotPool() {
  // LIFO: the slot freed last is the next one handed out.
  void *a = alloc::allocate<Role::Hot>(48, 16);
  alloc::deallocate<Role::Hot>(a, 48, 16);
  void *again = alloc::allocate<Role::Hot>(48, 16);
  check(again == a, "pools", "hot: a freed slot is reused first (LIFO)");
  alloc::deallocate<Role::Hot>(again, 48, 16);

  // Density: bumped slots of one class are adjacent, unless a page ran out
  // between them.
  const std::uint8_t index = classWithoutFreeSlots();
  const std::size_t slot = pools::slotBytes(index);
  const bool bumped = pools::hotClasses.classes[index].freeList == nullptr;
  auto *first = static_cast<std::byte *>(alloc::allocate<Role::Hot>(slot, 16));
  auto *second = static_cast<std::byte *>(alloc::allocate<Role::Hot>(slot, 16));
  const bool newPage = pageOf(second) != pageOf(first);
  check(bumped && (newPage || second == first + slot), "pools",
        "hot: consecutive %zu B slots are adjacent", slot);

  // Every block sits in a page of its own class, and classes don't share
  // pages.
  void *small = alloc::allocate<Role::Hot>(16, 16);
  void *large = alloc::allocate<Role::Hot>(1024, 16);
  check(pools::hotSegments.classOf(small) == pools::hotClassFor(16) &&
            pools::hotSegments.classOf(large) == pools::hotClassFor(1024) &&
            pools::hotSegments.classOf(first) == index &&
            pageOf(small) != pageOf(large),
        "pools", "hot: each block is in a page owned by its size class");

  // Classes share segments: a new segment is only taken when the last one is
  // full, so segments = ceil(pages / pagesPerSegment).
  std::uint64_t pages = 0;
  for (const pools::Segment &s : pools::hotSegments.used())
    pages += s.usedPages;
  const std::uint64_t perSegment = pools::pagesPerSegment;
  check(pools::hotSegments.count == (pages + perSegment - 1) / perSegment,
        "pools", "hot: %llu pages packed into %zu shared segments",
        static_cast<unsigned long long>(pages), pools::hotSegments.count);

  alloc::deallocate<Role::Hot>(large, 1024, 16);
  alloc::deallocate<Role::Hot>(small, 16, 16);
  alloc::deallocate<Role::Hot>(second, slot, 16);
  alloc::deallocate<Role::Hot>(first, slot, 16);
}

void checkArena() {
  pools::TransientArena &arena = pools::transientArena;
  if (arena.liveCount != 0) {
    check(false, "pools", "arena: no transient blocks live before the check");
    return;
  }

  // A live block skips the reset, so the arena keeps growing.
  const std::uint64_t resets = arena.resets;
  const std::uint64_t skipped = arena.skippedResets;
  void *live = alloc::allocate<Role::Transient>(64, 64);
  alloc::endPhase();
  void *next = alloc::allocate<Role::Transient>(64, 64);
  check(arena.skippedResets == skipped + 1 && arena.resets == resets &&
            next != live,
        "pools", "arena: a live block skips the reset");

  // Once everything is dead, the reset hands out chunk 0 from its start.
  alloc::deallocate<Role::Transient>(next, 64, 64);
  alloc::deallocate<Role::Transient>(live, 64, 64);
  alloc::endPhase();
  void *reused = alloc::allocate<Role::Transient>(64, 64);
  check(arena.resets == resets + 1 && reused == arena.chunks[0], "pools",
        "arena: an empty arena resets and reuses its memory");
  alloc::deallocate<Role::Transient>(reused, 64, 64);

  // More than a chunk's worth: blocks move on to the next chunk instead of
  // crossing its end.
  constexpr std::size_t blockBytes = 48 * 1024;
  std::vector<void *> blocks;
  for (std::size_t total = 0; total < 2 * pools::arenaChunkBytes;
       total += blockBytes)
    blocks.push_back(alloc::allocate<Role::Transient>(blockBytes, 64));
  bool inside = true;
  for (void *b : blocks) {
    const auto offset =
        reinterpret_cast<std::uintptr_t>(b) & (pools::arenaChunkBytes - 1);
    inside &= arena.owns(b) && offset + blockBytes <= pools::arenaChunkBytes;
  }
  check(inside && arena.current >= 1 && arena.wastedTailBytes > 0, "pools",
        "arena: %zu blocks of %zu B, none crossing a chunk end", blocks.size(),
        blockBytes);
  for (void *b : blocks)
    alloc::deallocate<Role::Transient>(b, blockBytes, 64);
  alloc::endPhase();
}

void checkRouting() {
  const pools::RoutingStats before = pools::routingStats;
  auto added = [&](const auto &field, Role role, pools::Route route) {
    const std::size_t r = alloc::index(role), i = std::to_underlying(route);
    return (pools::routingStats.*field)[r][i] - (before.*field)[r][i];
  };
  struct Request {
    Role role;
    std::size_t bytes, align;
    pools::Route route;
    void *p;
  };
  std::array<Request, 6> requests{{
      {Role::Hot, 16, 16, pools::Route::HotPool, nullptr},
      {Role::Hot, 2048, 16, pools::Route::Fallback, nullptr},
      {Role::Hot, 64, 64, pools::Route::Fallback, nullptr},
      {Role::Transient, 64, 64, pools::Route::Arena, nullptr},
      {Role::Transient, 128, 128, pools::Route::Fallback, nullptr},
      {Role::Cold, 16, 16, pools::Route::Fallback, nullptr},
  }};
  for (Request &q : requests)
    q.p = alloc::withRole(q.role, [&](auto r) {
      return alloc::allocate<decltype(r)::value>(q.bytes, q.align);
    });
  bool allocated = true, owned = true;
  for (const Request &q : requests) {
    allocated &= added(&pools::RoutingStats::allocations, q.role, q.route) >= 1;
    const bool inHot = pools::hotSegments.find(q.p) != nullptr;
    const bool inArena = pools::transientArena.owns(q.p);
    owned &= inHot == (q.route == pools::Route::HotPool) &&
             inArena == (q.route == pools::Route::Arena);
  }
  for (const Request &q : requests)
    alloc::withRole(q.role, [&](auto r) {
      alloc::deallocate<decltype(r)::value>(q.p, q.bytes, q.align);
    });
  bool freed = true;
  for (const Request &q : requests)
    freed &= added(&pools::RoutingStats::frees, q.role, q.route) ==
             added(&pools::RoutingStats::allocations, q.role, q.route);
  check(allocated && owned && freed, "pools",
        "routing: oversized and over-aligned requests fall back to glibc");
}

void checkPoolsStats() {
  const nlohmann::ordered_json j = alloc::Backend::extraStats();
  auto keysOf = [](const nlohmann::ordered_json &object) {
    std::vector<std::string> keys;
    for (const auto &item : object.items())
      keys.push_back(item.key());
    return keys;
  };
  using Keys = std::vector<std::string>;
  bool shape = keysOf(j) == Keys{"routing", "hot", "transient"} &&
               keysOf(j["routing"]) == Keys{"allocations", "frees"} &&
               keysOf(j["routing"]["allocations"]) ==
                   Keys{"default", "hot", "transient", "cold"} &&
               keysOf(j["routing"]["allocations"]["hot"]) ==
                   Keys{"hotPool", "arena", "fallback"} &&
               keysOf(j["hot"]) == Keys{"segments", "pagesUsed", "classes"} &&
               j["hot"]["classes"].is_array() && !j["hot"]["classes"].empty() &&
               keysOf(j["hot"]["classes"][0]) ==
                   Keys{"slotBytes", "pages",     "allocations",
                        "frees",     "liveSlots", "peakLiveSlots"} &&
               keysOf(j["transient"]) ==
                   Keys{"chunks",        "peakChunksUsed", "allocations",
                        "frees",         "liveBlocks",     "resets",
                        "skippedResets", "wastedTailBytes"};
  check(shape, "pools", "extraStats has every field, in order");
}

void checkPools() {
  checkHotPool();
  checkArena();
  checkRouting();
  checkPoolsStats();
}
#endif

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

// A backend with the optional checks (backend.hpp), to show the seam calls
// them before pools exists: it rejects every Cold free, and every allocation
// when rejectAllocations is set.
struct CheckingBackend {
  static constexpr std::string_view name = "checking";
  static inline bool rejectAllocations = false;
  template <Role R> static void *allocate(std::size_t bytes, std::size_t) {
    return ::operator new(bytes == 0 ? 16 : bytes);
  }
  template <Role R>
  static void deallocate(void *p, std::size_t, std::size_t) noexcept {
    ::operator delete(p);
  }
  static void endPhase() noexcept {}
  static nlohmann::ordered_json extraStats() { return {}; }
  template <Role R>
  static bool validAllocation(const void *, std::size_t, std::size_t) noexcept {
    return !rejectAllocations;
  }
  template <Role R>
  static bool validFree(const void *, std::size_t, std::size_t) noexcept {
    return R != Role::Cold;
  }
};
static_assert(alloc::SeamBackend<CheckingBackend> &&
              alloc::ChecksAllocations<CheckingBackend, Role::Hot> &&
              alloc::ChecksFrees<CheckingBackend, Role::Hot>);
static_assert(!alloc::ChecksFrees<alloc::PlainBackend, Role::Hot>);

#if defined(__linux__) || defined(__APPLE__)
template <class F>
void checkAborts(bool enabled, const char *what, const char *why, F f) {
  if (!enabled)
    skip("abort", "%s", why);
  else
    check(abortsInChild(f), "abort", "%s", what);
}

#if defined(TA_BACKEND_POOLS)
// Each mistake is set up so the seam's own checks pass (a block of the same
// size is live in the role it is freed as), so only the backend's check fires.
void checkPoolsAborts() {
  const char *noContracts = "contracts not enforced in this build";
  const char *noSeamChecks = "TA_SEAM_CHECKS is off";
  bool pre = contractsAbort();
  checkAborts(pre, "pools: hot free with the wrong size", noContracts, [] {
    void *p = alloc::allocate<Role::Hot>(48, 16);
    alloc::deallocate<Role::Hot>(p, 64, 16);
  });
  checkAborts(pre, "pools: hot free of an interior pointer", noContracts, [] {
    void *p = alloc::allocate<Role::Hot>(48, 16);
    alloc::deallocate<Role::Hot>(static_cast<std::byte *>(p) + 16, 32, 16);
  });
  checkAborts(pre && TA_SEAM_CHECKS != 0, "pools: hot double free",
              pre ? noSeamChecks : noContracts, [] {
                void *p = alloc::allocate<Role::Hot>(48, 16);
                (void)alloc::allocate<Role::Hot>(48, 16);
                alloc::deallocate<Role::Hot>(p, 48, 16);
                alloc::deallocate<Role::Hot>(p, 48, 16);
              });
  checkAborts(TA_SEAM_CHECKS != 0, "pools: hot slot written after free",
              noSeamChecks, [] {
                auto *p = static_cast<std::byte *>(
                    alloc::allocate<Role::Hot>(48, 16));
                alloc::deallocate<Role::Hot>(p, 48, 16);
                p[20] = std::byte{1};
                (void)alloc::allocate<Role::Hot>(48, 16);
              });
  checkAborts(pre, "pools: hot slot freed as cold", noContracts, [] {
    void *p = alloc::allocate<Role::Hot>(48, 16);
    (void)alloc::allocate<Role::Cold>(48, 16);
    alloc::deallocate<Role::Cold>(p, 48, 16);
  });
  checkAborts(pre, "pools: glibc block freed as hot", noContracts, [] {
    void *p = alloc::allocate<Role::Cold>(48, 16);
    (void)alloc::allocate<Role::Hot>(48, 16);
    alloc::deallocate<Role::Hot>(p, 48, 16);
  });
  checkAborts(pre, "pools: hot slot freed as transient", noContracts, [] {
    void *p = alloc::allocate<Role::Hot>(48, 16);
    (void)alloc::allocate<Role::Transient>(48, 16);
    alloc::deallocate<Role::Transient>(p, 48, 16);
  });
  checkAborts(pre, "pools: arena block freed as cold", noContracts, [] {
    void *p = alloc::allocate<Role::Transient>(48, 16);
    (void)alloc::allocate<Role::Cold>(48, 16);
    alloc::deallocate<Role::Cold>(p, 48, 16);
  });
}
#endif

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
  checkAborts(pre, "backend check: the backend rejects a free", noContracts,
              [] {
                void *p = alloc::allocate<Role::Cold, CheckingBackend>(16, 16);
                alloc::deallocate<Role::Cold, CheckingBackend>(p, 16, 16);
              });
  checkAborts(assertsAbort(), "backend check: the backend rejects a block",
              "assertions not checked in this build", [] {
                CheckingBackend::rejectAllocations = true;
                (void)alloc::allocate<Role::Hot, CheckingBackend>(16, 16);
              });
  checkAborts(assertsAbort(), "assertion: makeAs with an out-of-range role",
              "assertions not checked in this build",
              [] { (void)alloc::makeAs<int>(static_cast<Role>(7), 1); });
  checkAborts(
      TA_SEAM_CHECKS != 0, "seam check: allocation from a second thread",
      "TA_SEAM_CHECKS is off", [] {
        std::thread([] { (void)alloc::allocate<Role::Hot>(16, 16); }).join();
      });
#if defined(TA_BACKEND_POOLS)
  checkPoolsAborts();
#endif
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
#if defined(TA_BACKEND_POOLS)
  checkPools();
#endif
  checkContracts();
  check(seamStats.totalLiveBytes == 0, "leaks",
        "no seam bytes live at exit (%llu)",
        static_cast<unsigned long long>(seamStats.totalLiveBytes));

  return finish();
}
