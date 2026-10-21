#pragma once

#include "ta/contracts.hpp"

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace tatrace {
static_assert(std::endian::native == std::endian::little);

inline constexpr std::uint16_t formatVersion = 1;
inline constexpr char fileMagic[] = "TATRACE";

// starts at 1, so a zeroed slot reads as corrupt
enum class Kind : std::uint8_t {
  Alloc = 1,
  Free,
  // header for the following continuation slots
  Blob,
  Marker,
  ThreadStart,
  ThreadExit
};

// all entry points
enum class Api : std::uint8_t {
  None = 0,
  Malloc,
  Calloc,
  Realloc,
  Reallocarray,
  Memalign,
  PosixMemalign,
  AlignedAlloc,
  Valloc,
  Pvalloc,
  Free,
  New,
  NewArray,
  Delete,
  DeleteArray
};

enum class Flag : std::uint8_t {
  Nothrow = 1 << 0,
  Aligned = 1 << 1,
  Sized = 1 << 2,
  InPlace = 1 << 3,
  Bootstrap = 1 << 4,
  GlibcMmapped = 1 << 5,
  GlibcNonMainArena = 1 << 6
  // bit 7 is reserved
};

enum class BlobType : std::uint8_t { Stack = 1, String };

enum class MarkerKind : std::uint8_t {
  RegionBegin = 1,
  RegionEnd,
  Phase,
  LiveBytes
};

// on disk layout that the python reader accepts
struct Record {
  std::uint64_t tsc;
  std::uint64_t addr;
  std::uint64_t size;
  std::uint64_t stackId;
  Kind kind;
  Api api;
  std::uint8_t tag;
  std::uint8_t flags;
  std::uint32_t aux;
};

static_assert(offsetof(Record, tsc) == 0);
static_assert(offsetof(Record, addr) == 8);
static_assert(offsetof(Record, size) == 16);
static_assert(offsetof(Record, stackId) == 24);
static_assert(offsetof(Record, kind) == 32);
static_assert(offsetof(Record, api) == 33);
static_assert(offsetof(Record, tag) == 34);
static_assert(offsetof(Record, flags) == 35);
static_assert(offsetof(Record, aux) == 36);

static_assert(sizeof(Record) == 40);
static_assert(alignof(Record) == 8);
static_assert(std::is_trivially_copyable_v<Record>);
static_assert(std::is_standard_layout_v<Record>);

// flags

template <std::same_as<Flag>... Fs>
constexpr std::uint8_t flagBits(Fs... fs) noexcept {
  return static_cast<std::uint8_t>((std::to_underlying(fs) | ... | 0));
}

constexpr bool hasFlag(std::uint8_t flags, Flag f) noexcept {
  return (std::to_underlying(f) & flags) != 0;
}

// tag: bit 7 valid, bits 0-3 tier, bits 4-6 lifetime. without the valid bit,
// tier 0 (hot) would read as untagged.

inline constexpr std::uint8_t tagValid = 0x80;
inline constexpr std::uint8_t untaggedTag = 0;

constexpr std::uint8_t makeTag(unsigned tier, unsigned life) noexcept
    TA_PRE(tier < 16 && life < 8) {
  return static_cast<std::uint8_t>(tagValid | tier | life << 4);
}
constexpr bool isTagged(std::uint8_t tag) noexcept {
  return (tag & tagValid) != 0;
}
constexpr unsigned tagTier(std::uint8_t tag) noexcept TA_PRE(isTagged(tag)) {
  return tag & 0x0Fu;
}
constexpr unsigned tagLife(std::uint8_t tag) noexcept TA_PRE(isTagged(tag)) {
  return (tag >> 4) & 0x07u;
}

// blob sizing

inline constexpr std::size_t maxStackDepth = 32;
inline constexpr std::size_t maxBlobBytes = 280;
inline constexpr std::size_t maxBlobSlots = 8;

// header slot plus the continuation slots holding `bytes` of payload
constexpr std::size_t slotsFor(std::size_t bytes) noexcept {
  return 1 + (bytes + sizeof(Record) - 1) / sizeof(Record);
}

static_assert(slotsFor(maxBlobBytes) == maxBlobSlots);
static_assert(maxStackDepth * sizeof(std::uint64_t) <= maxBlobBytes);

// aux: a selector in the high byte, a value in the low 24 bits

inline constexpr unsigned auxHighShift = 24;
inline constexpr std::uint32_t auxLowMask = 0xFFFFFF;

// alignLog2 0 means no alignment was requested. slack saturates: a large
// value (e.g. an mmap'd chunk) is legitimate, not an error.
constexpr std::uint32_t packAlloc(unsigned alignLog2,
                                  std::uint64_t slack) noexcept
    TA_PRE(alignLog2 < 64) {
  return alignLog2 << auxHighShift |
         static_cast<std::uint32_t>(std::min(slack, std::uint64_t{auxLowMask}));
}
constexpr unsigned allocAlignLog2(std::uint32_t aux) noexcept {
  return aux >> auxHighShift;
}
constexpr std::uint32_t allocSlack(std::uint32_t aux) noexcept {
  return aux & auxLowMask;
}

// an oversized blob would overrun the slots the ring reserved
constexpr std::uint32_t packBlob(BlobType t, std::size_t bytes) noexcept
    TA_PRE(bytes <= maxBlobBytes) {
  return std::uint32_t{std::to_underlying(t)} << auxHighShift |
         static_cast<std::uint32_t>(bytes);
}
constexpr BlobType blobType(std::uint32_t aux) noexcept {
  return static_cast<BlobType>(aux >> auxHighShift);
}
constexpr std::size_t blobBytes(std::uint32_t aux) noexcept {
  return aux & auxLowMask;
}

constexpr std::uint32_t packMarker(MarkerKind k) noexcept {
  return std::uint32_t{std::to_underlying(k)} << auxHighShift;
}
constexpr MarkerKind markerKind(std::uint32_t aux) noexcept {
  return static_cast<MarkerKind>(aux >> auxHighShift);
}

// file layout: a FileHeader, then chunks. each chunk is a ChunkHeader and a
// payload padded to chunkAlign. readers skip tags they don't know, so new
// chunks don't bump formatVersion; changing an existing struct does.

static_assert(sizeof(fileMagic) == 8);

enum class HeaderFlag : std::uint16_t { InvariantTsc = 1 << 0 };

struct FileHeader {
  char magic[8];
  std::uint16_t version;
  std::uint16_t headerBytes; // readers skip fields past what they know
  std::uint16_t recordBytes;
  std::uint16_t headerFlags; // HeaderFlag bits
  std::uint32_t pid;
  std::uint32_t pageSize;
  std::uint64_t nominalHz;
  std::uint64_t startTicks;
  std::int64_t startMonotonicNs;
  std::int64_t startRealtimeNs; // display only
  std::uint64_t reserved;
};

static_assert(offsetof(FileHeader, magic) == 0);
static_assert(offsetof(FileHeader, version) == 8);
static_assert(offsetof(FileHeader, headerBytes) == 10);
static_assert(offsetof(FileHeader, recordBytes) == 12);
static_assert(offsetof(FileHeader, headerFlags) == 14);
static_assert(offsetof(FileHeader, pid) == 16);
static_assert(offsetof(FileHeader, pageSize) == 20);
static_assert(offsetof(FileHeader, nominalHz) == 24);
static_assert(offsetof(FileHeader, startTicks) == 32);
static_assert(offsetof(FileHeader, startMonotonicNs) == 40);
static_assert(offsetof(FileHeader, startRealtimeNs) == 48);
static_assert(offsetof(FileHeader, reserved) == 56);
static_assert(sizeof(FileHeader) == 64);

// 16-31 reserved for the sampler (step 5), 32-47 for PEBS (M4)
enum class ChunkTag : std::uint32_t { Meta = 1, RingData, Footer };

inline constexpr std::size_t chunkAlign = 8;

constexpr std::size_t paddedBytes(std::size_t bytes) noexcept {
  return (bytes + chunkAlign - 1) & ~(chunkAlign - 1);
}

struct ChunkHeader {
  ChunkTag tag;
  std::uint32_t reserved;
  std::uint64_t bytes; // payload, before padding
};

static_assert(offsetof(ChunkHeader, tag) == 0);
static_assert(offsetof(ChunkHeader, reserved) == 4);
static_assert(offsetof(ChunkHeader, bytes) == 8);
static_assert(sizeof(ChunkHeader) == 16);

// RingData payload: this, then records
struct RingDataHeader {
  std::uint32_t tid;
  std::uint32_t ringSerial;
  std::uint64_t firstSeq; // ring position of the first record, to find gaps
};

static_assert(offsetof(RingDataHeader, tid) == 0);
static_assert(offsetof(RingDataHeader, ringSerial) == 4);
static_assert(offsetof(RingDataHeader, firstSeq) == 8);
static_assert(sizeof(RingDataHeader) == 16);

// Footer payload: this, then ringCount RingStats
struct FooterHeader {
  std::uint64_t endTicks;
  std::int64_t endMonotonicNs;
  std::uint64_t untracedAfterTsc; // hooks past this pass through untraced
  std::uint64_t totalEvents;
  std::uint32_t ringCount;
  std::uint32_t reserved;
};

static_assert(offsetof(FooterHeader, endTicks) == 0);
static_assert(offsetof(FooterHeader, endMonotonicNs) == 8);
static_assert(offsetof(FooterHeader, untracedAfterTsc) == 16);
static_assert(offsetof(FooterHeader, totalEvents) == 24);
static_assert(offsetof(FooterHeader, ringCount) == 32);
static_assert(offsetof(FooterHeader, reserved) == 36);
static_assert(sizeof(FooterHeader) == 40);

struct RingStats {
  std::uint32_t serial;
  std::uint32_t tid;
  std::uint64_t events;
  std::uint64_t stallTicks;
  std::uint64_t stallCount;
};

static_assert(offsetof(RingStats, serial) == 0);
static_assert(offsetof(RingStats, tid) == 4);
static_assert(offsetof(RingStats, events) == 8);
static_assert(offsetof(RingStats, stallTicks) == 16);
static_assert(offsetof(RingStats, stallCount) == 24);
static_assert(sizeof(RingStats) == 32);

template <class T>
inline constexpr bool isOnDisk =
    std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

static_assert(isOnDisk<FileHeader> && isOnDisk<ChunkHeader> &&
              isOnDisk<RingDataHeader> && isOnDisk<FooterHeader> &&
              isOnDisk<RingStats>);

} // namespace tatrace
