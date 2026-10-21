#pragma once

// per-thread spsc ring of 40 B record slots. the producer is the allocation
// hook on the owning thread, the consumer is the watcher. one mmap per ring:
//
//   [ control: 3 x 128 B lines ][ capacity x 40 B slots ]
//
// ordering:
// 1. producer writes slots with plain stores, then release-stores head;
//    consumer acquire-loads head before reading any slot.
// 2. consumer finishes reading, then release-stores tail; producer
//    acquire-loads tail before overwriting a slot.
// 3. each side reads the other's line only on its slow path (cachedTail),
//    so the steady state doesn't ping-pong lines.
// 4. closed is released after the final head, and the watcher acquires it
//    before its last drain.
//
// fork: the child inherits rings with no watcher; handled by the shim's
// pthread_atfork, not here.

#include "ta/contracts.hpp"
#include "tatrace/format.hpp"
#include "tatrace/tsc.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>

#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>

namespace tatrace {

// a stack blob plus realloc's free and alloc, published together so a chunk
// never holds the free without its alloc.
inline constexpr std::uint32_t maxClaimSlots = maxBlobSlots + 2;
inline constexpr std::uint64_t minRingCapacity = 4 * maxClaimSlots;
inline constexpr std::uint64_t defaultRingCapacity = 65536; // 2.5 MiB
inline constexpr int spinIterations = 1000;
inline constexpr std::uint64_t ringMagic = 0x00474e4952544154; // "TATRING\0"
inline constexpr char ringVmaName[] = "tatrace-ring";

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

// the adjacent-line prefetcher pulls lines in pairs.
inline constexpr std::size_t ringLineBytes = 128;

// atomics only so the watcher can read stats while the thread still runs;
// the producer is the sole writer, so it uses plain load + store, no rmw.
struct alignas(ringLineBytes) RingProducerLine {
  std::atomic<std::uint64_t> head;
  std::uint64_t cachedTail;
  std::atomic<std::uint64_t> stallTicks;
  std::atomic<std::uint64_t> stallCount;
};

struct alignas(ringLineBytes) RingConsumerLine {
  std::atomic<std::uint64_t> tail;
};

struct alignas(ringLineBytes) RingFixedLine {
  std::uint64_t magic;
  std::uint64_t capacity;
  std::uint32_t tid;
  std::uint32_t serial;
  // written once, by the producer at thread exit
  std::atomic<std::uint32_t> closed;
};

struct RingControl {
  RingProducerLine producer;
  RingConsumerLine consumer;
  RingFixedLine fixed;
};

static_assert(sizeof(RingControl) == 3 * ringLineBytes);
static_assert(sizeof(RingControl) % alignof(Record) == 0);

inline Record *ringSlots(RingControl *control) noexcept {
  return reinterpret_cast<Record *>(control + 1);
}

inline std::size_t ringBytes(std::uint64_t capacity) noexcept {
  return sizeof(RingControl) + capacity * sizeof(Record);
}

[[gnu::always_inline]] inline void cpuRelax() noexcept {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#else
  asm volatile("yield");
#endif
}

// held by the owning thread (tls). only `control` is shared state.
class RingWriter {
public:
  explicit RingWriter(RingControl *control) noexcept
      TA_PRE(control != nullptr && control->fixed.magic == ringMagic)
      : control(control), slots(ringSlots(control)),
        mask(control->fixed.capacity - 1) {}

  // first seq of n free slots; blocks while the ring is full, never drops.
  std::uint64_t claim(std::uint32_t n) noexcept
      TA_PRE(n > 0 && n <= maxClaimSlots) {
    RingProducerLine &p = control->producer;
    std::uint64_t head = p.head.load(std::memory_order_relaxed);
    if (head + n - p.cachedTail <= mask + 1)
      return head;
    return claimSlow(head, n);
  }

  Record &slot(std::uint64_t seq) const noexcept { return slots[seq & mask]; }

  // blob payload across consecutive slots, re-masked per slot so it can
  // straddle the wrap. the last slot's tail is zeroed.
  void copyIn(std::uint64_t seq, const void *src,
              std::size_t bytes) const noexcept {
    auto *from = static_cast<const std::byte *>(src);
    for (; bytes > 0; ++seq) {
      std::size_t piece = std::min(bytes, sizeof(Record));
      auto *to = reinterpret_cast<std::byte *>(&slot(seq));
      std::memcpy(to, from, piece);
      std::memset(to + piece, 0, sizeof(Record) - piece);
      from += piece;
      bytes -= piece;
    }
  }

  void publish(std::uint64_t seq, std::uint32_t n) noexcept
      TA_PRE(n > 0 && n <= maxClaimSlots)
      TA_PRE(seq == control -> producer.head.load(std::memory_order_relaxed)) {
    control->producer.head.store(seq + n, std::memory_order_release);
  }

  // after the final publish
  void close() noexcept {
    control->fixed.closed.store(1, std::memory_order_release);
  }

private:
  [[gnu::noinline, gnu::cold]] std::uint64_t
  claimSlow(std::uint64_t head, std::uint32_t n) noexcept {
    RingProducerLine &p = control->producer;
    std::atomic<std::uint64_t> &tail = control->consumer.tail;
    std::uint64_t capacity = mask + 1;
    auto fits = [&] {
      p.cachedTail = tail.load(std::memory_order_acquire);
      return head + n - p.cachedTail <= capacity;
    };
    if (fits())
      return head;

    std::uint64_t start = readTsc();
    for (int spins = 0; !fits(); ++spins) {
      if (spins < spinIterations)
        cpuRelax();
      else
        ::sched_yield(); // the one producer syscall: the ring is full.
    }
    p.stallTicks.store(p.stallTicks.load(std::memory_order_relaxed) +
                           (readTsc() - start),
                       std::memory_order_relaxed);
    p.stallCount.store(p.stallCount.load(std::memory_order_relaxed) + 1,
                       std::memory_order_relaxed);
    return head;
  }

  RingControl *control;
  Record *slots;
  std::uint64_t mask;
};

// held by the watcher.
class RingView {
public:
  explicit RingView(RingControl *control) noexcept
      TA_PRE(control != nullptr && control->fixed.magic == ringMagic)
      : control(control), slots(ringSlots(control)),
        mask(control->fixed.capacity - 1) {}

  // everything published so far, as fn(a, b, firstSeq); b is non-empty only
  // when the range wraps. never a partial claim, so never a split blob.
  // returns the slot count.
  template <class F> std::uint64_t drain(F &&fn) {
    std::uint64_t head = control->producer.head.load(std::memory_order_acquire);
    std::uint64_t tail = control->consumer.tail.load(std::memory_order_relaxed);
    if (head == tail)
      return 0;
    std::uint64_t count = head - tail;
    std::uint64_t begin = tail & mask;
    std::uint64_t first = std::min(count, mask + 1 - begin);
    fn(std::span<const Record>(slots + begin, first),
       std::span<const Record>(slots, count - first), tail);
    control->consumer.tail.store(head, std::memory_order_release);
    return count;
  }

  std::uint64_t pending() const noexcept {
    return control->producer.head.load(std::memory_order_acquire) -
           control->consumer.tail.load(std::memory_order_relaxed);
  }

  // read before the last drain, so nothing published before close is missed.
  bool closed() const noexcept {
    return control->fixed.closed.load(std::memory_order_acquire) != 0;
  }

  // events counts slots, blob payload included; it equals head.
  RingStats stats() const noexcept {
    const RingProducerLine &p = control->producer;
    return {control->fixed.serial, control->fixed.tid,
            p.head.load(std::memory_order_acquire),
            p.stallTicks.load(std::memory_order_relaxed),
            p.stallCount.load(std::memory_order_relaxed)};
  }

  std::uint32_t tid() const noexcept { return control->fixed.tid; }
  std::uint32_t serial() const noexcept { return control->fixed.serial; }
  std::uint64_t capacity() const noexcept { return mask + 1; }

private:
  RingControl *control;
  const Record *slots;
  std::uint64_t mask;
};

// populated up front so the hook never page-faults on a slot; named so the
// smaps sum can exclude ring pages from the headline rss. a syscall: the
// caller decides when (first event, or pre-mapped by the watcher). null on
// failure; never allocates.
inline RingControl *mapRing(std::uint64_t capacity, std::uint32_t tid,
                            std::uint32_t serial) noexcept
    TA_PRE(std::has_single_bit(capacity) && capacity >= minRingCapacity) {
  std::size_t bytes = ringBytes(capacity);
  void *mem = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (mem == MAP_FAILED)
    return nullptr;
  // needs linux 5.17 and CONFIG_ANON_VMA_NAME; the ring works without it.
  ::prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, mem, bytes, ringVmaName);
  auto *control = new (mem) RingControl{};
  control->fixed.magic = ringMagic;
  control->fixed.capacity = capacity;
  control->fixed.tid = tid;
  control->fixed.serial = serial;
  return control;
}

// only after closed(), a final drain, and pending() == 0.
inline void unmapRing(RingControl *control) noexcept {
  if (control != nullptr)
    ::munmap(control, ringBytes(control->fixed.capacity));
}

} // namespace tatrace
