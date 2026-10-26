// Checks for tatrace/ring.hpp: publish and drain, filling to capacity, blob
// payload straddling the wrap, a full ring blocking the producer and counting
// the stall, the give-up hook ending that wait, reset for reuse, a two-thread
// stress run that checks continuity and that claims are never split, and the
// contracts. Also prints the uncontended cost of claim + publish. Exits
// nonzero if any check fails.

#include "ta/testing.hpp"
#include "tatrace/ring.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <random>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace ta::test;
using namespace tatrace;

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define TA_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define TA_SANITIZED 1
#endif
#endif

#if defined(TA_SANITIZED)
constexpr std::uint64_t stressSlots = 2'000'000;
#else
constexpr std::uint64_t stressSlots = 20'000'000;
#endif
constexpr std::uint64_t smallCapacity = 64;
constexpr std::uint64_t stressCapacity = 1024;
constexpr auto blockWindow = std::chrono::milliseconds{50};
constexpr int costRounds = 64;

Record makeRecord(std::uint64_t addr, std::uint64_t size = 0,
                  std::uint32_t aux = 0) {
  Record r{};
  r.kind = Kind::Alloc;
  r.addr = addr;
  r.size = size;
  r.aux = aux;
  return r;
}

// One slot per claim, addr = seq.
void fill(RingWriter &w, std::uint64_t count) {
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t s = w.claim(1);
    w.slot(s) = makeRecord(s);
    w.publish(s, 1);
  }
}

std::vector<Record> drainAll(RingView &v, std::uint64_t *firstSeq = nullptr,
                             std::size_t *wrapAt = nullptr) {
  std::vector<Record> out;
  v.drain([&](std::span<const Record> a, std::span<const Record> b,
              std::uint64_t first) {
    out.assign(a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    if (firstSeq)
      *firstSeq = first;
    if (wrapAt)
      *wrapAt = b.empty() ? 0 : a.size();
  });
  return out;
}

// Ring memory for one test. The pool supplies it in the real thing; here the
// heap does, which is fine because a test may allocate.
class TestRing {
public:
  explicit TestRing(std::uint64_t capacity)
      : mem(::operator new(ringBytes(capacity), ringAlign)),
        control(initRing(mem, capacity)) {}
  TestRing(const TestRing &) = delete;
  TestRing &operator=(const TestRing &) = delete;
  ~TestRing() { ::operator delete(mem, ringAlign); }

  RingControl *get() const { return control; }

private:
  static constexpr std::align_val_t ringAlign{alignof(RingControl)};
  void *mem;
  RingControl *control;
};

void checkBasic() {
  TestRing ring(smallCapacity);
  RingControl *c = ring.get();
  RingCounters fresh = RingView(c).counters();
  check(c->fixed.magic == ringMagic && c->fixed.capacity == smallCapacity &&
            fresh.events == 0 && fresh.stallCount == 0 && fresh.dropped == 0,
        "init", "initRing(%llu) starts empty with zeroed counters",
        static_cast<unsigned long long>(smallCapacity));
  RingWriter w(c);
  RingView v(c);

  std::uint64_t s = w.claim(1);
  w.slot(s) = makeRecord(7);
  w.publish(s, 1);
  std::uint64_t first = ~0ull;
  std::size_t wrapAt = 1;
  std::vector<Record> got = drainAll(v, &first, &wrapAt);
  check(s == 0 && got.size() == 1 && got[0].addr == 7 && first == 0 &&
            wrapAt == 0,
        "basic", "one slot: seq %llu, %zu drained, firstSeq %llu",
        static_cast<unsigned long long>(s), got.size(),
        static_cast<unsigned long long>(first));

  s = w.claim(3);
  for (std::uint64_t i = 0; i < 3; ++i)
    w.slot(s + i) = makeRecord(100 + i);
  w.publish(s, 3);
  got = drainAll(v, &first);
  bool inOrder = got.size() == 3;
  for (std::size_t i = 0; inOrder && i < 3; ++i)
    inOrder = got[i].addr == 100 + i;
  check(s == 1 && first == 1 && inOrder, "basic",
        "three slots in one claim drain in order from seq 1");

  check(v.drain([](auto, auto, auto) {}) == 0 && v.pending() == 0, "basic",
        "drain of an empty ring returns 0");
}

void checkFull() {
  TestRing ring(smallCapacity);
  RingControl *c = ring.get();
  RingWriter w(c);
  RingView v(c);
  fill(w, smallCapacity - 1);
  check(v.pending() == smallCapacity - 1, "full", "%llu of %llu slots pending",
        static_cast<unsigned long long>(v.pending()),
        static_cast<unsigned long long>(smallCapacity));
  fill(w, 1);
  RingCounters st = v.counters();
  check(v.pending() == smallCapacity && st.stallCount == 0, "full",
        "the last free slot claims without a stall (%llu stalls)",
        static_cast<unsigned long long>(st.stallCount));
  std::vector<Record> got = drainAll(v);
  bool intact = got.size() == smallCapacity;
  for (std::size_t i = 0; intact && i < got.size(); ++i)
    intact = got[i].addr == i;
  check(intact && st.events == smallCapacity, "full",
        "a full ring drains all %zu slots intact", got.size());
}

// A 270 B blob claimed at seq 60 of 64: header at 60, payload 61..67 wraps.
void checkStraddle() {
  TestRing ring(smallCapacity);
  RingControl *c = ring.get();
  RingWriter w(c);
  RingView v(c);
  fill(w, 60);
  drainAll(v);

  constexpr std::size_t bytes = 270;
  std::array<std::byte, bytes> payload;
  for (std::size_t i = 0; i < bytes; ++i)
    payload[i] = static_cast<std::byte>(i * 7 + 3);
  auto payloadSlots = static_cast<std::uint32_t>(slotsFor(bytes) - 1);
  std::uint32_t n = 1 + payloadSlots + 1;

  std::uint64_t s = w.claim(n);
  Record header = makeRecord(0xb10b, 0, packBlob(BlobType::Stack, bytes));
  header.kind = Kind::Blob;
  w.slot(s) = header;
  w.copyIn(s + 1, payload.data(), bytes);
  w.slot(s + 1 + payloadSlots) = makeRecord(0xa110c, 48);
  w.publish(s, n);

  std::uint64_t first = 0;
  std::size_t wrapAt = 0;
  std::vector<Record> got = drainAll(v, &first, &wrapAt);
  check(got.size() == n && first == 60 && wrapAt == 4, "straddle",
        "%zu slots from seq %llu, wrapping after %zu", got.size(),
        static_cast<unsigned long long>(first), wrapAt);
  if (got.size() != n)
    return;

  auto *raw = reinterpret_cast<const std::byte *>(got.data() + 1);
  std::size_t padding = payloadSlots * sizeof(Record) - bytes;
  bool zeroed = true;
  for (std::size_t i = 0; i < padding; ++i)
    zeroed = zeroed && raw[bytes + i] == std::byte{0};
  check(got[0].kind == Kind::Blob && blobBytes(got[0].aux) == bytes &&
            std::memcmp(raw, payload.data(), bytes) == 0 && zeroed,
        "straddle", "%zu B payload reassembles across the wrap, %zu B pad zero",
        bytes, padding);
  check(got[n - 1].addr == 0xa110c && got[n - 1].size == 48, "straddle",
        "the alloc after the blob lands intact");
}

void checkBlocking() {
  TestRing ring(smallCapacity);
  RingControl *c = ring.get();
  RingWriter w(c);
  RingView v(c);
  fill(w, smallCapacity);

  std::atomic<bool> done = false;
  std::thread producer([&] {
    fill(w, 1);
    done.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(blockWindow);
  bool blocked = !done.load(std::memory_order_acquire);
  check(blocked && v.counters().stallCount == 0, "blocking",
        "claim on a full ring is still blocked after %lld ms",
        static_cast<long long>(blockWindow.count()));

  std::vector<Record> first = drainAll(v);
  producer.join();
  std::vector<Record> second = drainAll(v);
  RingCounters st = v.counters();
  check(first.size() == smallCapacity && second.size() == 1 &&
            second[0].addr == smallCapacity,
        "blocking", "after a drain the blocked claim lands as seq %llu",
        static_cast<unsigned long long>(second.empty() ? 0 : second[0].addr));
  check(st.stallCount == 1 && st.stallTicks > 0, "blocking",
        "one stall of %llu ticks recorded",
        static_cast<unsigned long long>(st.stallTicks));
}

// Each slot holds addr = seq, size = its claim's length, aux = its index in
// the claim, so the consumer can check continuity and that every chunk starts
// at a claim boundary.
void checkStress() {
  TestRing ring(stressCapacity);
  RingControl *c = ring.get();
  RingWriter w(c);
  RingView v(c);
  // Stands in for the pool's Closed state: released after the last publish,
  // acquired before the final drain.
  std::atomic<bool> producerDone = false;

  std::thread producer([&] {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<std::uint32_t> sizes(1, maxClaimSlots);
    for (std::uint64_t written = 0; written < stressSlots;) {
      std::uint32_t n = sizes(rng);
      std::uint64_t s = w.claim(n);
      for (std::uint32_t i = 0; i < n; ++i)
        w.slot(s + i) = makeRecord(s + i, n, i);
      w.publish(s, n);
      written += n;
    }
    producerDone.store(true, std::memory_order_release);
  });

  std::uint64_t expected = 0, chunks = 0, gaps = 0, splits = 0, torn = 0;
  for (;;) {
    bool closed = producerDone.load(std::memory_order_acquire);
    std::uint64_t got = v.drain([&](std::span<const Record> a,
                                    std::span<const Record> b,
                                    std::uint64_t first) {
      ++chunks;
      gaps += first != expected;
      splits += a.front().aux != 0;
      std::uint32_t inClaim = 0, claimLen = 0;
      for (std::span<const Record> part : {a, b})
        for (const Record &r : part) {
          if (inClaim == claimLen) {
            inClaim = 0;
            claimLen = static_cast<std::uint32_t>(r.size);
          }
          torn += r.addr != expected || r.aux != inClaim || r.size != claimLen;
          ++inClaim;
          ++expected;
        }
      splits += inClaim != claimLen;
    });
    if (closed && got == 0)
      break;
    if (got == 0)
      std::this_thread::yield();
  }
  producer.join();
  RingCounters st = v.counters();

  check(expected >= stressSlots && gaps == 0 && torn == 0, "stress",
        "%llu slots in %llu drains: %llu gap(s), %llu bad slot(s)",
        static_cast<unsigned long long>(expected),
        static_cast<unsigned long long>(chunks),
        static_cast<unsigned long long>(gaps),
        static_cast<unsigned long long>(torn));
  check(splits == 0, "stress", "no claim split across drains (%llu)",
        static_cast<unsigned long long>(splits));
  check(st.events == expected, "stress", "stats count %llu slots",
        static_cast<unsigned long long>(st.events));
  std::printf("INFO  stress    %llu stalls, %llu ticks stalled\n",
              static_cast<unsigned long long>(st.stallCount),
              static_cast<unsigned long long>(st.stallTicks));
}

bool alwaysGiveUp(const void *) noexcept { return true; }

// Counts its calls and gives up on the third.
bool giveUpOnThird(const void *ctx) noexcept {
  auto *calls = static_cast<int *>(const_cast<void *>(ctx));
  return ++*calls >= 3;
}

void checkGiveUp() {
  TestRing ring(smallCapacity);
  RingControl *c = ring.get();
  RingWriter w(c, {alwaysGiveUp, nullptr});
  RingView v(c);
  fill(w, smallCapacity);

  std::uint64_t s = w.claim(1);
  RingCounters st = v.counters();
  check(s == claimFailed && v.pending() == smallCapacity && st.dropped == 1 &&
            st.stallCount == 1,
        "giveup",
        "a full ring returns claimFailed, publishes nothing, counts one drop "
        "and one stall");
  s = w.claim(maxClaimSlots);
  check(s == claimFailed && v.counters().dropped == 2, "giveup",
        "the next full claim fails too; a multi-slot claim is one drop");

  drainAll(v);
  s = w.claim(1);
  check(s == smallCapacity && v.counters().dropped == 2, "giveup",
        "after a drain, claims succeed again at seq %llu",
        static_cast<unsigned long long>(s));

  int calls = 0;
  TestRing counted(smallCapacity);
  RingWriter cw(counted.get(), {giveUpOnThird, &calls});
  RingView cv(counted.get());
  fill(cw, smallCapacity);
  s = cw.claim(1);
  st = cv.counters();
  check(s == claimFailed && calls == 3 && st.stallTicks > 0 && st.dropped == 1,
        "giveup",
        "the hook is asked each round until it says stop (%d calls), and the "
        "stall time is still recorded",
        calls);

  int quiet = 0;
  TestRing roomy(smallCapacity);
  RingWriter rw(roomy.get(), {giveUpOnThird, &quiet});
  fill(rw, smallCapacity);
  check(quiet == 0, "giveup", "the hook isn't asked while there's room");
}

void checkReset() {
  TestRing ring(smallCapacity);
  RingControl *c = ring.get();
  RingWriter w(c, {alwaysGiveUp, nullptr});
  RingView v(c);
  fill(w, smallCapacity);
  static_cast<void>(w.claim(1));
  drainAll(v);
  fill(w, 5);
  drainAll(v);

  resetRing(c);
  RingCounters st = v.counters();
  check(st.events == 0 && st.stallCount == 0 && st.stallTicks == 0 &&
            st.dropped == 0 && v.pending() == 0 && c->producer.cachedTail == 0,
        "reset", "resetRing zeroes head, tail and every counter");

  // A new writer, as the ring's next owner would make.
  RingWriter next(c);
  fill(next, 3);
  std::uint64_t first = ~0ull;
  std::vector<Record> got = drainAll(v, &first);
  check(first == 0 && got.size() == 3 && got[0].addr == 0 && got[2].addr == 2,
        "reset", "the next producer starts again from seq 0");
}

// Volatile, so the calls happen at run time where contracts are checked.
volatile std::uint32_t runtimeZero = 0;
volatile std::uint32_t runtimeTooMany = maxClaimSlots + 1;
volatile std::uint64_t runtimeOddCapacity = 100;
volatile std::uint64_t runtimeWrongSeq = 5;
volatile std::size_t runtimeMisalign = 8;

void checkContracts() {
  if (!contractsAbort()) {
    skip("contract", "contracts not enforced in this build");
    return;
  }
  TestRing ring(smallCapacity);
  RingControl *c = ring.get();
  check(abortsInChild([c] {
          RingWriter w(c);
          static_cast<void>(w.claim(runtimeZero));
        }),
        "contract", "claim(0) aborts");
  check(abortsInChild([c] {
          RingWriter w(c);
          static_cast<void>(w.claim(runtimeTooMany));
        }),
        "contract", "claim(maxClaimSlots + 1) aborts");
  check(abortsInChild(
            [c] { static_cast<void>(initRing(c, runtimeOddCapacity)); }),
        "contract",
        "initRing with a capacity that isn't a power of two aborts");
  check(abortsInChild([c] {
          auto *odd = reinterpret_cast<std::byte *>(c) + runtimeMisalign;
          static_cast<void>(initRing(odd, smallCapacity));
        }),
        "contract", "initRing on memory not aligned to a line aborts");
  check(abortsInChild([c] {
          RingWriter w(c);
          fill(w, 1);
          resetRing(c);
        }),
        "contract", "resetRing with undrained slots aborts");
  check(abortsInChild([c] {
          RingWriter w(c);
          static_cast<void>(w.claim(1));
          w.publish(runtimeWrongSeq, 1);
        }),
        "contract", "publish out of order aborts");
}

// Informational: claim + publish of one slot with room in the ring. Timed per
// round between drains, so the drain isn't counted.
void printCost() {
  TestRing ring(defaultRingCapacity);
  RingControl *c = ring.get();
  RingWriter w(c);
  RingView v(c);
  ta::TickSample start = ta::samplePair();
  std::uint64_t ticks = 0;
  for (int round = 0; round < costRounds; ++round) {
    std::uint64_t t0 = readTsc();
    fill(w, defaultRingCapacity);
    ticks += readTsc() - t0;
    v.drain([](auto, auto, auto) {});
  }
  double rate = ticksPerNs(start, ta::samplePair());
  double ns = static_cast<double>(ticks) / rate /
              static_cast<double>(costRounds * defaultRingCapacity);
  std::printf("INFO  cost      claim + write + publish %.2f ns per slot\n", ns);
}

} // namespace

int main() {
  checkBasic();
  checkFull();
  checkStraddle();
  checkBlocking();
  checkStress();
  checkGiveUp();
  checkReset();
  checkContracts();
  printCost();
  return finish();
}
