#pragma once

// hand-written pools/arenas for hot/transient types; delegates to glibc for
// everything else. used to check performance of hinted tieralloc against what
// is manually done today.

#include "backend.hpp"
#include "harness/contracts.hpp"
#include "plain.hpp"
#include "role.hpp"
#include "stats.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <span>
#include <string_view>
#include <utility>

namespace tabench::alloc::pools {
inline constexpr std::size_t hotClassStep = 16;
inline constexpr std::size_t hotMaxBytes = 1024;
inline constexpr std::size_t hotClassCount = hotMaxBytes / hotClassStep;
// 2 MiB
inline constexpr std::size_t segmentBytes = 2 * 1024 * 1024;
// 64 KiB
inline constexpr std::size_t pageBytes = 64 * 1024;
inline constexpr std::size_t pagesPerSegment = segmentBytes / pageBytes;
inline constexpr std::size_t maxSegments = 1024;
// 64 KiB
inline constexpr std::size_t transientMaxBytes = 64 * 1024;
inline constexpr std::size_t transientMaxAlign = 64;
// 1 MiB
inline constexpr std::size_t arenaChunkBytes = 1 * 1024 * 1024;

inline constexpr std::size_t maxChunks = 1024;

static_assert(transientMaxBytes + transientMaxAlign <= arenaChunkBytes,
              "one maximal transient request must fit in a single chunk");

enum class Route : std::uint8_t { HotPool, Arena, Fallback };
inline constexpr std::size_t routeCount = 3;
static_assert(std::to_underlying(Route::Fallback) + 1 == routeCount);
inline constexpr std::array<const char *, routeCount> routeNames{
    "hotPool", "arena", "fallback"};

template <Role R>
constexpr Route route(std::size_t bytes, std::size_t align) noexcept {
  switch (R) {
  case Role::Hot:
    if (bytes <= hotMaxBytes && align <= hotClassStep)
      return Route::HotPool;
    else
      return Route::Fallback;
  case Role::Transient:
    if (bytes <= transientMaxBytes && align <= transientMaxAlign)
      return Route::Arena;
    else
      return Route::Fallback;
  case Role::Cold:
  case Role::Default:
    return Route::Fallback;
  }
  std::unreachable();
}

// segment metadata
struct Segment {
  std::byte *base;
  // class index or freePage
  std::array<std::uint8_t, pagesPerSegment> pageClass;
  std::uint8_t usedPages;
};

inline constexpr std::uint8_t freePage = 0xff;

struct HotSegments {
  std::array<Segment, maxSegments> entries{};
  // entries [0, count) are live
  std::size_t count{0};

  std::byte *newPage(std::uint8_t classIndex) {
    Segment &s = (count == 0 || entries[count - 1].usedPages == pagesPerSegment)
                     ? newSegment()
                     : entries[count - 1];
    s.pageClass[s.usedPages] = classIndex;
    return s.base + std::size_t{s.usedPages++} * pageBytes;
  }

  // The live segment holding p, or nullptr if p is not in a hot segment.
  const Segment *find(const void *p) const noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(p) & ~(segmentBytes - 1);
    for (const Segment &s : used())
      if (reinterpret_cast<std::uintptr_t>(s.base) == base)
        return &s;
    return nullptr;
  }

  // The class owning p's page
  std::uint8_t classOf(const void *p) const noexcept {
    const Segment *s = find(p);
    if (s == nullptr)
      return freePage;
    const auto offset = reinterpret_cast<std::uintptr_t>(p) -
                        reinterpret_cast<std::uintptr_t>(s->base);
    return s->pageClass[offset / pageBytes];
  }

  std::span<const Segment> used() const noexcept {
    return {entries.data(), count};
  }

private:
  // Registers a new segment. The entry is written only once the memory is
  // obtained, so a bad_alloc leaves the registry unchanged.
  Segment &newSegment() {
    if (count == maxSegments)
      throw std::bad_alloc();
    auto *base = static_cast<std::byte *>(
        std::aligned_alloc(segmentBytes, segmentBytes));
    if (base == nullptr)
      throw std::bad_alloc();
    Segment &s = entries[count++];
    s.base = base;
    s.pageClass.fill(freePage);
    s.usedPages = 0;
    return s;
  }
};

inline constinit HotSegments hotSegments{};

// HotClasses turns pages into slots and recycles them. It is the path taken for
// hot objects. Allocations are done with a bump pointer (and only request a new
// segment when its page is fully used up), and a free puts the slot back on its
// class's free list. Objects get segregated into 16-byte steps and placed in
// their respective class.

// Class i holds slots of (i + 1) * hotClassStep bytes, for requests of
// (i * hotClassStep, (i + 1) * hotClassStep] bytes; 0 bytes goes to class 0.
constexpr std::uint8_t hotClassFor(std::size_t bytes) noexcept {
  if (bytes == 0)
    return 0;
  return static_cast<std::uint8_t>((bytes - 1) / hotClassStep);
}

constexpr std::size_t slotBytes(std::uint8_t index) noexcept {
  return (std::size_t{index} + 1) * hotClassStep;
}

// What a free slot holds: the free-list link, and under TA_SEAM_CHECKS a marker
// and 0xdd poison in the rest of the slot.
struct FreeSlot {
  FreeSlot *next;
  std::uintptr_t marker;
};
static_assert(sizeof(FreeSlot) <= hotClassStep,
              "a free slot must fit in the smallest class");

inline constexpr std::uintptr_t freeMarker = 0xdeadf4ee5107f4ee;
inline constexpr unsigned char poisonByte = 0xdd;

struct HotClass {
  FreeSlot *freeList;
  std::byte *cursor;
  std::byte *end;
  std::uint64_t pages;
  std::uint64_t allocations;
  std::uint64_t frees;
  std::uint64_t peakLive;
};

struct HotClasses {
  std::array<HotClass, hotClassCount> classes{};

  [[nodiscard, gnu::always_inline]] void *allocate(std::size_t bytes) {
    const std::uint8_t index = hotClassFor(bytes);
    HotClass &c = classes[index];
    void *p;
    if (c.freeList != nullptr) {
      FreeSlot *slot = c.freeList;
      c.freeList = slot->next;
#if TA_SEAM_CHECKS
      checkPoison(slot, index, bytes);
      slot->marker = 0;
#endif
      p = slot;
    } else if (c.cursor != c.end) {
      p = c.cursor;
      c.cursor += slotBytes(index);
    } else {
      p = refill(c, index);
    }
    ++c.allocations;
    if (const std::uint64_t live = c.allocations - c.frees; live > c.peakLive)
      c.peakLive = live;
    return p;
  }

  [[gnu::always_inline]] void deallocate(void *p, std::size_t bytes) noexcept {
    const std::uint8_t index = hotClassFor(bytes);
    HotClass &c = classes[index];
    FreeSlot *slot =
        std::construct_at(static_cast<FreeSlot *>(p), c.freeList, 0);
#if TA_SEAM_CHECKS
    std::memset(reinterpret_cast<std::byte *>(slot) + sizeof(FreeSlot),
                poisonByte, slotBytes(index) - sizeof(FreeSlot));
    slot->marker = freeMarker;
#endif
    c.freeList = slot;
    ++c.frees;
  }

  // Whether p is the start of a slot in a page owned by the class for bytes.
  bool validBlock(const void *p, std::size_t bytes) const noexcept {
    const std::uint8_t index = hotClassFor(bytes);
    if (hotSegments.classOf(p) != index)
      return false;
    // Pages are pageBytes-aligned, so masking finds p's page.
    return (reinterpret_cast<std::uintptr_t>(p) & (pageBytes - 1)) %
               slotBytes(index) ==
           0;
  }

  // Whether p carries the free marker
  bool isFree(const void *p) const noexcept {
#if TA_SEAM_CHECKS
    return static_cast<const FreeSlot *>(p)->marker == freeMarker;
#else
    static_cast<void>(p);
    return false;
#endif
  }

  // The classes used so far; the backend adds the segment totals.
  nlohmann::ordered_json toJson() const {
    nlohmann::ordered_json j = nlohmann::ordered_json::array();
    for (std::size_t i = 0; i < hotClassCount; ++i) {
      const HotClass &c = classes[i];
      if (c.pages == 0)
        continue;
      j.push_back({{"slotBytes", slotBytes(static_cast<std::uint8_t>(i))},
                   {"pages", c.pages},
                   {"allocations", c.allocations},
                   {"frees", c.frees},
                   {"liveSlots", c.allocations - c.frees},
                   {"peakLiveSlots", c.peakLive}});
    }
    return j;
  }

private:
  // Runs once per page of a class, so it stays out of the inlined allocate.
  // c changes only after newPage returns, so a bad_alloc leaves it unchanged.
  [[gnu::noinline]] std::byte *refill(HotClass &c, std::uint8_t index) {
    std::byte *page = hotSegments.newPage(index);
    const std::size_t slot = slotBytes(index);
    c.end = page + (pageBytes / slot) * slot;
    c.cursor = page + slot;
    ++c.pages;
    return page;
  }

#if TA_SEAM_CHECKS
  // A popped slot must still hold the poison deallocate wrote; anything else
  // means a write after free.
  static void checkPoison(const FreeSlot *slot, std::uint8_t index,
                          std::size_t bytes) noexcept {
    const auto *bytesOf = reinterpret_cast<const unsigned char *>(slot);
    for (std::size_t i = sizeof(FreeSlot); i < slotBytes(index); ++i)
      if (bytesOf[i] != poisonByte)
        detail::seamCheckFailed("hot slot written after free", Role::Hot,
                                bytes);
    if (slot->marker != freeMarker)
      detail::seamCheckFailed("hot slot written after free", Role::Hot, bytes);
  }
#endif
};

inline constinit HotClasses hotClasses{};

struct TransientArena {
  // [0, chunkCount) is obtained; chunks[current] is being bumped.
  std::array<std::byte *, maxChunks> chunks{};
  std::size_t chunkCount{0};
  std::size_t current{0};
  std::byte *cursor = nullptr;
  std::byte *end = nullptr;
  std::uint64_t liveCount{0};

  // extra stats
  std::uint64_t allocations{0};
  std::uint64_t frees{0};
  std::uint64_t resets{0};
  std::uint64_t skippedResets{0};
  std::uint64_t wastedTailBytes{0};
  std::size_t peakChunksUsed{0};

  // fast path
  [[nodiscard, gnu::always_inline]] void *allocate(std::size_t bytes,
                                                   std::size_t align) {
    // 0 bytes still takes align bytes, so consecutive blocks stay distinct.
    const std::size_t b = bytes == 0 ? align : bytes;
    const auto addr = reinterpret_cast<std::uintptr_t>(cursor);
    const auto limit = reinterpret_cast<std::uintptr_t>(end);
    const std::uintptr_t aligned = (addr + align - 1) & ~(align - 1);
    std::byte *p;
    if (aligned <= limit && b <= limit - aligned) {
      p = cursor + (aligned - addr);
    } else {
      nextChunk();
      p = cursor;
    }
    cursor = p + b;
    ++liveCount;
    ++allocations;
    return p;
  }

  // under TA_SEAM_CHECKS will poison block
  [[gnu::always_inline]] void deallocate(void *p, std::size_t bytes) noexcept {
    --liveCount;
    ++frees;
#if TA_SEAM_CHECKS
    std::memset(p, poisonByte, bytes);
#else
    static_cast<void>(p);
    static_cast<void>(bytes);
#endif
  }

  void endPhase() noexcept {
    if (liveCount != 0) {
      ++skippedResets;
      return;
    }
#if TA_SEAM_CHECKS
    poisonUsed();
#endif
    current = 0;
    cursor = chunkCount == 0 ? nullptr : chunks[0];
    end = chunkCount == 0 ? nullptr : cursor + arenaChunkBytes;
    ++resets;
  }

  // whether p lies inside one of the obtained chunks. Chunks are
  // arenaChunkBytes-aligned, so masking finds p's chunk.
  bool owns(const void *p) const noexcept {
    const auto base =
        reinterpret_cast<std::uintptr_t>(p) & ~(arenaChunkBytes - 1);
    for (std::byte *chunk : used())
      if (reinterpret_cast<std::uintptr_t>(chunk) == base)
        return true;
    return false;
  }

  // chunks obtained so far
  std::span<std::byte *const> used() const noexcept {
    return {chunks.data(), chunkCount};
  }

  nlohmann::ordered_json toJson() const {
    nlohmann::ordered_json j;
    j["chunks"] = chunkCount;
    j["peakChunksUsed"] = peakChunksUsed;
    j["allocations"] = allocations;
    j["frees"] = frees;
    j["liveBlocks"] = liveCount;
    j["resets"] = resets;
    j["skippedResets"] = skippedResets;
    j["wastedTailBytes"] = wastedTailBytes;
    return j;
  }

private:
  // Moves to the next chunk, obtaining it if no earlier phase did. Leaves the
  // arena unchanged if it throws.
  [[gnu::noinline]] void nextChunk() {
    const std::size_t next = cursor == nullptr ? 0 : current + 1;
    if (next == chunkCount) {
      if (chunkCount == maxChunks)
        throw std::bad_alloc();
      auto *chunk = static_cast<std::byte *>(
          std::aligned_alloc(arenaChunkBytes, arenaChunkBytes));
      if (chunk == nullptr)
        throw std::bad_alloc();
      chunks[chunkCount++] = chunk;
    }
    wastedTailBytes += static_cast<std::size_t>(end - cursor);
    current = next;
    cursor = chunks[next];
    end = cursor + arenaChunkBytes;
    if (next + 1 > peakChunksUsed)
      peakChunksUsed = next + 1;
  }

#if TA_SEAM_CHECKS
  void poisonUsed() noexcept {
    if (cursor == nullptr)
      return;
    for (std::size_t i = 0; i < current; ++i)
      std::memset(chunks[i], poisonByte, arenaChunkBytes);
    std::memset(chunks[current], poisonByte,
                static_cast<std::size_t>(cursor - chunks[current]));
  }
#endif
};

inline constinit TransientArena transientArena{};

// Where each role's requests went. Counted in timing builds too: if many Hot
// requests fall back, the pools numbers measure something else, and the
// summary has to show it.
struct RoutingStats {
  std::array<std::array<std::uint64_t, routeCount>, roleCount> allocations{};
  std::array<std::array<std::uint64_t, routeCount>, roleCount> frees{};

  nlohmann::ordered_json toJson() const {
    nlohmann::ordered_json j;
    j["allocations"] = table(allocations);
    j["frees"] = table(frees);
    return j;
  }

private:
  static nlohmann::ordered_json
  table(const std::array<std::array<std::uint64_t, routeCount>, roleCount>
            &counts) {
    nlohmann::ordered_json j;
    for (std::size_t r = 0; r < roleCount; ++r) {
      nlohmann::ordered_json &row = j[toString(static_cast<Role>(r))];
      for (std::size_t i = 0; i < routeCount; ++i)
        row[routeNames[i]] = counts[r][i];
    }
    return j;
  }
};

inline constinit RoutingStats routingStats{};

// Routes each request to the hot pool, the arena or glibc (through
// PlainBackend, so the 0-byte and over-alignment rules are plain's).
struct PoolsBackend {
  static constexpr std::string_view name = "pools";

  template <Role R>
  [[nodiscard, gnu::always_inline]] static void *allocate(std::size_t bytes,
                                                          std::size_t align) {
    const Route r = route<R>(bytes, align);
    void *p = nullptr;
    switch (r) {
    case Route::HotPool:
      p = hotClasses.allocate(bytes);
      break;
    case Route::Arena:
      p = transientArena.allocate(bytes, align);
      break;
    case Route::Fallback:
      p = PlainBackend::allocate<R>(bytes, align);
      break;
    }
    ++routingStats.allocations[index(R)][std::to_underlying(r)];
    return p;
  }

  template <Role R>
  [[gnu::always_inline]] static void deallocate(void *p, std::size_t bytes,
                                                std::size_t align) noexcept {
    const Route r = route<R>(bytes, align);
    switch (r) {
    case Route::HotPool:
      hotClasses.deallocate(p, bytes);
      break;
    case Route::Arena:
      transientArena.deallocate(p, bytes);
      break;
    case Route::Fallback:
      PlainBackend::deallocate<R>(p, bytes, align);
      break;
    }
    ++routingStats.frees[index(R)][std::to_underlying(r)];
  }

  static void endPhase() noexcept { transientArena.endPhase(); }

  static nlohmann::ordered_json extraStats() {
    std::uint64_t pagesUsed = 0;
    for (const Segment &s : hotSegments.used())
      pagesUsed += s.usedPages;
    nlohmann::ordered_json j;
    j["routing"] = routingStats.toJson();
    j["hot"]["segments"] = hotSegments.count;
    j["hot"]["pagesUsed"] = pagesUsed;
    j["hot"]["classes"] = hotClasses.toJson();
    j["transient"] = transientArena.toJson();
    return j;
  }

  // The seam's check hooks (backend.hpp). Identical for pools: a block the
  // backend produced and a block handed back must both belong to their route.
  template <Role R>
  static bool validAllocation(const void *p, std::size_t bytes,
                              std::size_t align) noexcept {
    return belongs<R>(p, bytes, align);
  }

  template <Role R>
  static bool validFree(const void *p, std::size_t bytes,
                        std::size_t align) noexcept {
    return belongs<R>(p, bytes, align);
  }

private:
  // Whether p belongs to the route of this request. Searches the segment and
  // chunk tables, so only check builds may call it.
  template <Role R>
  static bool belongs(const void *p, std::size_t bytes,
                      std::size_t align) noexcept {
    switch (route<R>(bytes, align)) {
    case Route::HotPool:
      // !isFree is the double-free check, so validFree must run before
      // deallocate writes the marker; the seam's TA_PRE guarantees it.
      return hotClasses.validBlock(p, bytes) && !hotClasses.isFree(p);
    case Route::Arena:
      return transientArena.owns(p);
    case Route::Fallback:
      return hotSegments.find(p) == nullptr && !transientArena.owns(p);
    }
    std::unreachable();
  }
};
static_assert(SeamBackend<PoolsBackend>);
// A hook whose signature drifts silently turns the checks off (backend.hpp).
static_assert([]<std::size_t... I>(std::index_sequence<I...>) {
  return ((ChecksAllocations<PoolsBackend, static_cast<Role>(I)> &&
           ChecksFrees<PoolsBackend, static_cast<Role>(I)>) &&
          ...);
}(std::make_index_sequence<roleCount>{}));

} // namespace tabench::alloc::pools
