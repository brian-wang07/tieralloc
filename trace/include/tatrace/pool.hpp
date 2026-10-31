#pragma once

// the shared-memory pool of rings: one memfd, created by the watcher process,
// mapped by the shim in the traced process.
//
//   [ header: 3 x 128 B lines ][ ring 0 ][ ring 1 ] ... [ ring n-1 ]
//
// each ring's stride is the pool's line for it, then the ring itself:
//
//   [ PoolRingLine: state, tid, serial ][ RingControl ][ slots ]
//
// the ring knows nothing about owners; who holds it and when it's recycled
// live in the PoolRingLine. the header and every stride start on a page
// boundary. only offsets and values cross between the processes, never
// pointers: each side maps the pool at its own address.

#include "ta/contracts.hpp"
#include "ta/ticks.hpp"
#include "tatrace/ring.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tatrace {

inline constexpr std::uint64_t poolMagic = 0x004c4f4f50544154; // "TATPOOL\0"
inline constexpr std::uint32_t poolVersion = 3;
// launcher -> shim: the pool's fd number in the target
inline constexpr char poolFdEnv[] = "TATRACE_FD";
// shows as /memfd:tatrace-pool in the target's maps and smaps
inline constexpr char poolMemfdName[] = "tatrace-pool";
inline constexpr std::uint32_t defaultRingCount = 64;
inline constexpr std::uint32_t maxRingCount = 4096;
// 640 MiB per ring maximum
inline constexpr std::uint64_t maxRingCapacity = std::uint64_t{1} << 24;
// the watcher beats every pass (~1 ms), so this only expires when it's gone
inline constexpr std::uint64_t defaultHeartbeatTimeoutNs = 2'000'000'000;

static_assert(std::bit_cast<std::uint64_t>(std::array<char, 8>{
                  'T', 'A', 'T', 'P', 'O', 'O', 'L', '\0'}) == poolMagic);

// watcher writes it once at create; read-only after.
struct alignas(ringLineBytes) PoolFixedLine {
  // keep the prefix the same. every build reads at these specific offsets, so
  // attachPool can tell a different build's pool from a stranger's file before
  // trusting
  std::uint64_t
      magic; // offset 0: written last, so a valid magic means a complete pool
  std::uint32_t version; // offset 8
  std::uint32_t
      watcherPid; // offset 12: the target's parent, and others dont match

  // rest is only read when the prefix is verified
  std::uint32_t pageSize;
  std::uint32_t ringCount;
  // slots per ring, a power of two
  std::uint64_t ringCapacity;
  // the layout as created; attach checks it against layoutFor
  std::uint64_t headerBytes;
  std::uint64_t ringStride;
  std::uint64_t totalBytes;
  // catch a shim and watcher built from different sources
  std::uint32_t headerStructBytes;
  std::uint32_t ringControlBytes;
  std::uint32_t recordBytes;
  std::uint32_t poolRingLineBytes;
};
static_assert(offsetof(PoolFixedLine, magic) == 0);
static_assert(offsetof(PoolFixedLine, version) == 8);
static_assert(offsetof(PoolFixedLine, watcherPid) == 12);

// how a producer blocked on a full ring tells a live watcher from a dead
// one; the pool's give-up hook reads it.
struct PoolLiveness {
  // watcher: CLOCK_MONOTONIC at its last pass
  std::atomic<std::uint64_t> heartbeatNs;
  // watcher: no more drains are coming
  std::atomic<std::uint32_t> stopping;
  // watcher, once at create
  std::uint64_t timeoutNs;
};

// watcher writes; producers read it only on the slow path.
struct alignas(ringLineBytes) PoolWatcherLine {
  PoolLiveness liveness;
};

// shim writes; watcher reads.
struct alignas(ringLineBytes) PoolShimLine {
  // cas 0 -> pid, so exactly one process attaches
  std::atomic<std::uint32_t> attachedPid;
  // ring serials, unique within the pool
  std::atomic<std::uint32_t> nextSerial;
  // where the next claim scan starts
  std::atomic<std::uint32_t> claimHint;
  // threads that found no free ring
  std::atomic<std::uint64_t> disabledThreads;
  // events after the thread's ring closed (late dtors, etc)
  std::atomic<std::uint64_t> lateEvents;
};

// one writer per line, like the ring's control block: the heartbeat is
// rewritten every pass and mustn't share a line with the claim counters.
struct PoolHeader {
  PoolFixedLine fixed;
  PoolWatcherLine watcher;
  PoolShimLine shim;
};

static_assert(sizeof(PoolHeader) == 3 * ringLineBytes);
static_assert(alignof(PoolHeader) == ringLineBytes);
static_assert(offsetof(PoolHeader, watcher) == ringLineBytes);
static_assert(offsetof(PoolHeader, shim) == 2 * ringLineBytes);
// fits the first page on any system
static_assert(sizeof(PoolHeader) <= 4096);
// lives in shared memory: no destructor ever runs on it, and both processes
// must agree on where every field is
static_assert(std::is_standard_layout_v<PoolHeader>);
static_assert(std::is_trivially_destructible_v<PoolHeader>);
// a lock-free atomic works on the memory word itself, so it works across
// processes; a lock-based one would lock inside one process only
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

namespace detail {

constexpr std::size_t roundUp(std::size_t value, std::size_t align) noexcept
    TA_PRE(std::has_single_bit(align)) {
  return (value + align - 1) & ~(align - 1);
}

// raii guards
// close the fd unless release() hands it forward
class FdGuard {
public:
  explicit FdGuard(int fd) noexcept : fd(fd) {}
  FdGuard(const FdGuard &) = delete;
  FdGuard &operator=(const FdGuard &) = delete;
  ~FdGuard() {
    if (fd >= 0)
      ::close(fd);
  }
  int get() const noexcept { return fd; }
  int release() noexcept { return std::exchange(fd, -1); }

private:
  int fd;
};

// unmaps unless release() hands the mapping forward
class MapGuard {
public:
  MapGuard(void *addr, std::size_t bytes) noexcept : addr(addr), bytes(bytes) {}
  MapGuard(const MapGuard &) = delete;
  MapGuard &operator=(const MapGuard &) = delete;
  ~MapGuard() {
    if (addr != MAP_FAILED)
      ::munmap(addr, bytes);
  }
  explicit operator bool() const noexcept { return addr != MAP_FAILED; }
  void *get() const noexcept { return addr; }
  void *release() noexcept { return std::exchange(addr, MAP_FAILED); }

private:
  void *addr;
  std::size_t bytes;
};

inline int openPoolMemfd() noexcept {
  unsigned flags = MFD_CLOEXEC | MFD_ALLOW_SEALING;
#ifdef MFD_NOEXEC_SEAL
  int fd = ::memfd_create(poolMemfdName, flags | MFD_NOEXEC_SEAL);
  if (fd >= 0 || errno != EINVAL)
    return fd;
#endif
  return ::memfd_create(poolMemfdName, flags);
}

} // namespace detail

// Free is 0, so a zero-filled memfd reads as all rings free.
//
//          claimRing (cas)       tid, serial written      closeRing
//   Free ──────────────▶ Claiming ───────────────▶ Active ─────────▶ Closed
//    ▲                                                                   │
//    └─────────── recycleRing: final drain, resetRing, store Free ───────┘
enum class RingState : std::uint32_t {
  Free,
  Claiming,
  Active,
  Closed,
};

// ordering on state:
// - claimRing's cas Free -> Claiming acquires, pairing with recycleRing's
//   release of Free, so the new owner sees the reset ring.
// - Active and Closed are released by the shim and acquired by the watcher,
//   so it sees tid and serial before draining, and every publish before its
//   final drain.
// - Claiming exists so the watcher never sees Active before tid and serial.
struct alignas(ringLineBytes) PoolRingLine {
  std::atomic<RingState> state;
  // written only while Claiming, by the claiming thread
  std::uint32_t tid;
  std::uint32_t serial;
};

static_assert(sizeof(PoolRingLine) == ringLineBytes);
static_assert(std::is_standard_layout_v<PoolRingLine>);
static_assert(std::is_trivially_destructible_v<PoolRingLine>);
static_assert(std::atomic<RingState>::is_always_lock_free);

struct PoolLayout {
  std::size_t headerBytes;
  std::size_t ringStride;
  std::size_t totalBytes;

  bool operator==(const PoolLayout &) const = default;
};

constexpr bool validRingShape(std::uint32_t ringCount,
                              std::uint64_t capacity) noexcept {
  return ringCount > 0 && ringCount <= maxRingCount &&
         std::has_single_bit(capacity) && capacity >= minRingCapacity &&
         capacity <= maxRingCapacity;
}

constexpr PoolLayout layoutFor(std::uint32_t ringCount, std::uint64_t capacity,
                               std::size_t pageSize) noexcept
    TA_PRE(validRingShape(ringCount, capacity))
    TA_PRE(std::has_single_bit(pageSize) && pageSize >= alignof(RingControl)) {
  std::size_t headerBytes = detail::roundUp(sizeof(PoolHeader), pageSize);
  std::size_t ringStride =
      detail::roundUp(sizeof(PoolRingLine) + ringBytes(capacity), pageSize);
  std::size_t totalBytes = headerBytes + ringCount * ringStride;
  return {headerBytes, ringStride, totalBytes};
}

// one process's mapping of the pool. a plain handle: copied freely, owns
// nothing. offsets come from the recorded header, which attach has already
// checked against layoutFor.
struct Pool {
  PoolHeader *header = nullptr;
  std::byte *base = nullptr;
  std::size_t bytes = 0;

  std::uint32_t ringCount() const noexcept { return header->fixed.ringCount; }

  PoolRingLine *ringLine(std::uint32_t i) const noexcept
      TA_PRE(i < ringCount()) {
    return reinterpret_cast<PoolRingLine *>(base + header->fixed.headerBytes +
                                            i * header->fixed.ringStride);
  }

  RingControl *ring(std::uint32_t i) const noexcept TA_PRE(i < ringCount()) {
    return reinterpret_cast<RingControl *>(ringLine(i) + 1);
  }

  PoolLiveness *liveness() const noexcept { return &header->watcher.liveness; }
};

struct PoolConfig {
  std::uint32_t ringCount = defaultRingCount;
  std::uint64_t ringCapacity = defaultRingCapacity;
  std::uint64_t heartbeatTimeoutNs = defaultHeartbeatTimeoutNs;
};

struct CreatedPool {
  Pool pool;
  int fd = -1;
};

struct PoolError {
  const char *step;
  int error;
};

inline std::expected<CreatedPool, PoolError>
createPool(const PoolConfig &config) noexcept {
  auto fail = [](const char *msg, int err = errno) {
    return std::unexpected(PoolError{msg, err});
  };

  // zero would make every full ring give up at once: drops, not blocking
  if (!validRingShape(config.ringCount, config.ringCapacity) ||
      config.heartbeatTimeoutNs == 0)
    return fail("config", EINVAL);
  std::size_t pageSize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  PoolLayout layout =
      layoutFor(config.ringCount, config.ringCapacity, pageSize);
  detail::FdGuard fd(detail::openPoolMemfd());
  if (fd.get() < 0)
    return fail("memfd_create");
  if (::ftruncate(fd.get(), static_cast<off_t>(layout.totalBytes)) != 0)
    return fail("ftruncate");
  if (::fcntl(fd.get(), F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0)
    return fail("fcntl");
  detail::MapGuard map(::mmap(nullptr, layout.totalBytes,
                              PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                              fd.get(), 0),
                       layout.totalBytes);
  if (!map)
    return fail("mmap");

  auto *header = new (map.get()) PoolHeader{};
  PoolFixedLine &fixed = header->fixed;
  fixed.version = poolVersion;
  fixed.ringCount = config.ringCount;
  fixed.ringCapacity = config.ringCapacity;
  fixed.headerBytes = layout.headerBytes;
  fixed.ringStride = layout.ringStride;
  fixed.totalBytes = layout.totalBytes;
  fixed.pageSize = static_cast<std::uint32_t>(pageSize);
  // the launcher forks the target, so this is the target's getppid()
  fixed.watcherPid = static_cast<std::uint32_t>(::getpid());
  fixed.headerStructBytes = sizeof(PoolHeader);
  fixed.ringControlBytes = sizeof(RingControl);
  fixed.recordBytes = sizeof(Record);
  fixed.poolRingLineBytes = sizeof(PoolRingLine);

  PoolLiveness &liveness = header->watcher.liveness;
  liveness.timeoutNs = config.heartbeatTimeoutNs;
  liveness.heartbeatNs.store(static_cast<std::uint64_t>(ta::monotonicNs()),
                             std::memory_order_relaxed);

  Pool pool{header, static_cast<std::byte *>(map.get()), layout.totalBytes};
  for (std::uint32_t i = 0; i < pool.ringCount(); ++i) {
    // the memfd is already zero; constructing both makes them real objects
    // and doesn't lean on that
    new (pool.ringLine(i)) PoolRingLine{};
    initRing(pool.ring(i), config.ringCapacity);
  }

  // written last: whoever acquires a valid magic sees a complete pool
  std::atomic_ref(fixed.magic).store(poolMagic, std::memory_order_release);

  map.release();
  return CreatedPool{pool, fd.release()};
}

inline void destroyPool(CreatedPool &created) noexcept {
  if (created.pool.base)
    ::munmap(created.pool.base, created.pool.bytes);
  if (created.fd >= 0)
    ::close(created.fd);
  created = CreatedPool{};
}

inline void beat(const Pool &pool) {
  pool.header->watcher.liveness.heartbeatNs.store(
      static_cast<std::uint64_t>(ta::monotonicNs()), std::memory_order_relaxed);
}

inline void stop(const Pool &pool) {
  // called after final drain, so release to let producer know that nothing else
  // will be drained.
  pool.header->watcher.liveness.stopping.store(1, std::memory_order_release);
}

inline std::uint32_t attachedPid(const Pool &pool) {
  return pool.header->shim.attachedPid.load(std::memory_order_acquire);
}

inline std::uint64_t disabledThreads(const Pool &pool) {
  return pool.header->shim.disabledThreads.load(std::memory_order_relaxed);
}

inline std::uint64_t lateEvents(const Pool &pool) {
  return pool.header->shim.lateEvents.load(std::memory_order_relaxed);
}

// BadPool: not a tatrace pool, usually a grandchild whose fd number now names
// an unrelated file; stay silent.
// Mismatch: magic matches, but from a different build. worth reporting to user
// as otherwise would leave an empty trace with no explanation
enum class AttachError {
  NoPool,
  BadPool,
  Mismatch,
  NotOurWatcher,
  AlreadyAttached,
  MapFailed,
};

inline int parsePoolFd(const char *value) noexcept {
  // from_chars takes a leading '-' for int, so "-0" would parse as 0
  if (value == nullptr || *value < '0' || *value > '9')
    return -1;
  const char *end = value + std::strlen(value);
  int fd = -1;
  auto [ptr, ec] = std::from_chars(value, end, fd);
  if (ec != std::errc{} || ptr != end)
    return -1;
  return fd;
}

namespace detail {

// the fixed line comes from another process, or from whatever file sits at
// the fd number, so every field is checked before any of them reaches
// layoutFor's contract
constexpr bool fixedLineMatches(const PoolFixedLine &fixed,
                                std::size_t pageSize,
                                std::size_t fileBytes) noexcept {
  if (fixed.version != poolVersion)
    return false;
  if (fixed.headerStructBytes != sizeof(PoolHeader) ||
      fixed.ringControlBytes != sizeof(RingControl) ||
      fixed.recordBytes != sizeof(Record) ||
      fixed.poolRingLineBytes != sizeof(PoolRingLine))
    return false;
  if (fixed.pageSize != pageSize ||
      !validRingShape(fixed.ringCount, fixed.ringCapacity))
    return false;
  PoolLayout recorded{fixed.headerBytes, fixed.ringStride, fixed.totalBytes};
  return layoutFor(fixed.ringCount, fixed.ringCapacity, pageSize) == recorded &&
         fixed.totalBytes == fileBytes;
}

} // namespace detail

inline std::expected<Pool, AttachError> attachPool(int fd) noexcept {
  struct stat st;
  constexpr int required = F_SEAL_SHRINK | F_SEAL_GROW;
  std::size_t pageSize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  if (fd < 0)
    return std::unexpected(AttachError::NoPool);
  if (::fstat(fd, &st) == -1)
    return std::unexpected(AttachError::NoPool);
  if (int seals = ::fcntl(fd, F_GET_SEALS);
      seals == -1 || (seals & required) != required)
    return std::unexpected(AttachError::BadPool);
  if (static_cast<std::size_t>(st.st_size) < pageSize)
    return std::unexpected(AttachError::BadPool);

  PoolFixedLine fixed{};
  {
    detail::MapGuard map(
        ::mmap(nullptr, pageSize, PROT_READ, MAP_SHARED, fd, 0), pageSize);
    if (!map)
      return std::unexpected(AttachError::BadPool);
    auto *header = static_cast<const PoolHeader *>(map.get());
    std::uint64_t magic =
        std::atomic_ref(header->fixed.magic).load(std::memory_order_acquire);
    if (magic != poolMagic)
      return std::unexpected(AttachError::BadPool);
    fixed = header->fixed;
  }
  // the parent check comes before the layout check, so only the target can
  // report Mismatch, otherwise its grandchildren will inherit the open fd and
  // would print it again. watcherPid is in the fixed prefix, so its safe to
  // read before the layout is verified.
  if (static_cast<std::uint32_t>(::getppid()) != fixed.watcherPid)
    return std::unexpected(AttachError::NotOurWatcher);
  if (!detail::fixedLineMatches(fixed, pageSize,
                                static_cast<std::size_t>(st.st_size)))
    return std::unexpected(AttachError::Mismatch);

  detail::MapGuard map(::mmap(nullptr, fixed.totalBytes, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_POPULATE, fd, 0),
                       fixed.totalBytes);
  // errno is still mmap's here: the guard holds MAP_FAILED, so nothing runs
  // on the way out. the shim reads it for its message.
  if (!map)
    return std::unexpected(AttachError::MapFailed);
  auto *header = static_cast<PoolHeader *>(map.get());
  std::uint32_t expected = 0;
  if (!header->shim.attachedPid.compare_exchange_strong(
          expected, static_cast<std::uint32_t>(::getpid()),
          std::memory_order_acq_rel, std::memory_order_acquire))
    return std::unexpected(AttachError::AlreadyAttached);
  ::close(fd);
  return Pool{header, static_cast<std::byte *>(map.release()),
              fixed.totalBytes};
}

namespace detail {
inline bool watcherGone(const void *ctx) noexcept {
  const auto *liveness = static_cast<const PoolLiveness *>(ctx);
  if (liveness->stopping.load(std::memory_order_acquire) != 0)
    return true;
  std::uint64_t heartbeat =
      liveness->heartbeatNs.load(std::memory_order_relaxed);
  auto now = static_cast<std::uint64_t>(ta::monotonicNs());
  return heartbeat < now && now - heartbeat > liveness->timeoutNs;
}
} // namespace detail

inline RingGiveUp poolGiveUp(const Pool &pool) noexcept {
  return {&detail::watcherGone, pool.liveness()};
}

struct ClaimedRing {
  std::uint32_t index;
  RingControl *ring;
};

inline std::optional<ClaimedRing> claimRing(const Pool &pool,
                                            std::uint32_t tid) noexcept {
  std::uint32_t ringCount = pool.ringCount();
  std::uint32_t start =
      pool.header->shim.claimHint.load(std::memory_order_relaxed) % ringCount;
  for (std::uint32_t k = 0; k < ringCount; ++k) {
    std::uint32_t i = (start + k) % ringCount;
    PoolRingLine *ringLine = pool.ringLine(i);
    if (ringLine->state.load(std::memory_order_relaxed) != RingState::Free)
      continue;
    RingState expected = RingState::Free;
    if (!ringLine->state.compare_exchange_strong(expected, RingState::Claiming,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed))
      continue;
    ringLine->tid = tid;
    ringLine->serial =
        pool.header->shim.nextSerial.fetch_add(1, std::memory_order_relaxed);
    ringLine->state.store(RingState::Active, std::memory_order_release);
    pool.header->shim.claimHint.store(i + 1, std::memory_order_relaxed);
    return ClaimedRing{i, pool.ring(i)};
  }
  pool.header->shim.disabledThreads.fetch_add(1, std::memory_order_relaxed);
  return std::nullopt;
}

inline void closeRing(const Pool &pool, std::uint32_t index) noexcept
    TA_PRE(index < pool.ringCount())
    TA_PRE(pool.ringLine(index)
               -> state.load(std::memory_order_relaxed) == RingState::Active) {
  pool.ringLine(index)->state.store(RingState::Closed,
                                    std::memory_order_release);
}

inline RingState ringState(const Pool &pool, std::uint32_t index) noexcept {
  return pool.ringLine(index)->state.load(std::memory_order_acquire);
}

inline void recycleRing(const Pool &pool, std::uint32_t index) noexcept
    TA_PRE(ringState(pool, index) == RingState::Closed) {

  resetRing(pool.ring(index));

  pool.ringLine(index)->tid = 0;
  pool.ringLine(index)->serial = 0;
  pool.ringLine(index)->state.store(RingState::Free, std::memory_order_release);
}

inline RingStats ringStats(const Pool &pool, std::uint32_t index) noexcept {
  const PoolRingLine &line = *pool.ringLine(index);
  RingCounters counters = RingView(pool.ring(index)).counters();
  return {line.serial,         line.tid,
          counters.events,     counters.stallTicks,
          counters.stallCount, counters.dropped};
}
} // namespace tatrace
