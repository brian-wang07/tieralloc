#pragma once

// per-thread spsc ring of 40 B record slots. the producer is the allocation
// hook on the owning thread, the consumer is the watcher. the memory comes
// from the caller (the pool), so the ring knows nothing about who owns it,
// when it's recycled, or whether the watcher is alive:
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
// 4. resetRing orders nothing itself; whoever hands the ring to its next
//    producer publishes the zeroes (the pool's release store of Free).

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

namespace tatrace {

// a stack blob plus realloc's free and alloc, published together so a chunk
// never holds the free without its alloc.
inline constexpr std::uint32_t maxClaimSlots = maxBlobSlots + 2;
inline constexpr std::uint64_t minRingCapacity = 4 * maxClaimSlots;
inline constexpr std::uint64_t defaultRingCapacity = 65536; // 2.5 MiB
inline constexpr int spinIterations = 1000;
inline constexpr std::uint64_t ringMagic = 0x00474e4952544154; // "TATRING\0"
// what claim returns when the give-up hook ended the wait
inline constexpr std::uint64_t claimFailed = ~std::uint64_t{0};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

// the adjacent-line prefetcher pulls lines in pairs.
inline constexpr std::size_t ringLineBytes = 128;

struct alignas(ringLineBytes) RingProducerLine {
  std::atomic<std::uint64_t> head;
  std::uint64_t cachedTail;
  std::atomic<std::uint64_t> stallTicks;
  std::atomic<std::uint64_t> stallCount;
  std::atomic<std::uint64_t> dropped;
};

struct alignas(ringLineBytes) RingConsumerLine {
  std::atomic<std::uint64_t> tail;
};

struct alignas(ringLineBytes) RingFixedLine {
  std::uint64_t magic;
  std::uint64_t capacity;
};

struct RingControl {
  RingProducerLine producer;
  RingConsumerLine consumer;
  RingFixedLine fixed;
};

static_assert(sizeof(RingControl) == 3 * ringLineBytes);
static_assert(sizeof(RingControl) % alignof(Record) == 0);

// watcher adds tid and serial from the pool to make the footer's RingStats
struct RingCounters {
  std::uint64_t events;
  std::uint64_t stallTicks;
  std::uint64_t stallCount;
  std::uint64_t dropped;
};

// asked while a producer waits on a full ring: true ends the wait with
// claimFailed
struct RingGiveUp {
  bool (*fn)(const void *ctx) noexcept = nullptr;
  const void *ctx = nullptr;
};

namespace detail {

inline Record *ringSlots(RingControl *control) noexcept {
  return reinterpret_cast<Record *>(control + 1);
}

} // namespace detail

inline constexpr std::size_t ringBytes(std::uint64_t capacity) noexcept {
  return sizeof(RingControl) + capacity * sizeof(Record);
}

// mem must hold ringBytes(capacity). the ring starts empty with zeroed
// counters
inline RingControl *initRing(void *mem, std::uint64_t capacity) noexcept
    TA_PRE(mem != nullptr &&
           reinterpret_cast<std::uintptr_t>(mem) % alignof(RingControl) == 0)
    TA_PRE(std::has_single_bit(capacity) && capacity >= minRingCapacity) {
  auto *control = new (mem) RingControl{};
  control->fixed.magic = ringMagic;
  control->fixed.capacity = capacity;
  return control;
}

// back to empty with zeroed counters, for the ring's next producer. the
// caller guarantees no producer is attached and the ring is drained.
inline void resetRing(RingControl *control) noexcept
    TA_PRE(control != nullptr && control -> fixed.magic == ringMagic)
    TA_PRE(control->producer.head.load(std::memory_order_relaxed) ==
           control->consumer.tail.load(std::memory_order_relaxed)) {
  RingProducerLine &p = control->producer;
  p.head.store(0, std::memory_order_relaxed);
  p.cachedTail = 0;
  p.stallTicks.store(0, std::memory_order_relaxed);
  p.stallCount.store(0, std::memory_order_relaxed);
  p.dropped.store(0, std::memory_order_relaxed);
  control->consumer.tail.store(0, std::memory_order_relaxed);
}

namespace detail {

[[gnu::always_inline]] inline void cpuRelax() noexcept {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#else
  asm volatile("yield");
#endif
}

} // namespace detail

// held by the owning thread (tls). only `control` is shared state.
class RingWriter {
public:
  explicit RingWriter(RingControl *control, RingGiveUp giveUp = {}) noexcept
      TA_PRE(control != nullptr && control->fixed.magic == ringMagic)
      : control(control), slots(detail::ringSlots(control)),
        mask(control->fixed.capacity - 1), giveUp(giveUp) {}

  // first seq of n free slots. blocks while the ring is full, until the
  // give-up hook says stop: then claimFailed, and the caller skips the event.
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
    std::uint64_t result = head;
    for (int spins = 0; !fits(); ++spins) {
      if (spins < spinIterations) {
        detail::cpuRelax();
        continue;
      }
      if (giveUp.fn != nullptr && giveUp.fn(giveUp.ctx)) {
        add(p.dropped, 1);
        result = claimFailed;
        break;
      }
      ::sched_yield(); // the one producer syscall: the ring is full.
    }
    add(p.stallTicks, readTsc() - start);
    add(p.stallCount, 1);
    return result;
  }

  // the producer is the only writer, so no rmw.
  static void add(std::atomic<std::uint64_t> &counter,
                  std::uint64_t by) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + by,
                  std::memory_order_relaxed);
  }

  RingControl *control;
  Record *slots;
  std::uint64_t mask;
  RingGiveUp giveUp;
};

// held by the watcher.
class RingView {
public:
  explicit RingView(RingControl *control) noexcept
      TA_PRE(control != nullptr && control->fixed.magic == ringMagic)
      : control(control), slots(detail::ringSlots(control)),
        mask(control->fixed.capacity - 1) {}

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

  RingCounters counters() const noexcept {
    const RingProducerLine &p = control->producer;
    return {p.head.load(std::memory_order_acquire),
            p.stallTicks.load(std::memory_order_relaxed),
            p.stallCount.load(std::memory_order_relaxed),
            p.dropped.load(std::memory_order_relaxed)};
  }

  std::uint64_t capacity() const noexcept { return mask + 1; }

private:
  RingControl *control;
  const Record *slots;
  std::uint64_t mask;
};

} // namespace tatrace
