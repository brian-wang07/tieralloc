// Checks for tatrace/pool.hpp. The layout arithmetic is checked at compile
// time. At run time, a pool laid out by hand in heap memory checks where each
// ring lands and passes records through every ring; a real memfd pool from
// createPool checks the header, the seals, two mappings of one fd, config
// rejection, destroyPool and the watcher's helpers. attachPool is checked
// from forked children and grandchildren: what it turns away (and that the
// fd stays open), tampered headers that must not abort, a real attach the
// watcher reads back, and each way a second attach fails. The ring lifecycle
// is checked last: the give-up hook, claims in sequence and racing threads,
// close and recycle, and a child that claims, writes and closes a ring the
// watcher drains. Also checks that the contracts abort. Exits nonzero if any
// check fails.

#include "ta/testing.hpp"
#include "tatrace/pool.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace tatrace;
using namespace ta::test;

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define TA_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define TA_SANITIZED 1
#endif
#endif

// the defaults with 4 KiB pages: the ring line doesn't move the stride, since
// 128 + 384 + 2 621 440 B rounds up to 641 pages either way
constexpr PoolLayout defaults = layoutFor(64, defaultRingCapacity, 4096);
static_assert(defaults.headerBytes == 4096);
static_assert(defaults.ringStride == 2'625'536);
static_assert(defaults.totalBytes == 168'038'400);

// 64 KiB pages (some aarch64 kernels)
constexpr PoolLayout bigPages = layoutFor(64, defaultRingCapacity, 65536);
static_assert(bigPages.headerBytes == 65536);
static_assert(bigPages.ringStride == 41 * 65536);

// the smallest power of two that holds minRingCapacity fits one page
constexpr std::uint64_t smallestCapacity = std::bit_ceil(minRingCapacity);
static_assert(layoutFor(1, smallestCapacity, 4096).ringStride == 4096);

// the bounds keep the arithmetic far from overflow
static_assert(layoutFor(maxRingCount, maxRingCapacity, 65536).totalBytes <
              std::size_t{1} << 48);

// fixedLineMatches: a line as createPool writes it passes, and each field
// that attach relies on fails on its own, without tripping layoutFor
constexpr PoolFixedLine fixedFor(std::uint32_t ringCount,
                                 std::uint64_t capacity, std::size_t pageSize) {
  PoolLayout l = layoutFor(ringCount, capacity, pageSize);
  PoolFixedLine f{};
  f.magic = poolMagic;
  f.version = poolVersion;
  f.ringCount = ringCount;
  f.ringCapacity = capacity;
  f.headerBytes = l.headerBytes;
  f.ringStride = l.ringStride;
  f.totalBytes = l.totalBytes;
  f.pageSize = static_cast<std::uint32_t>(pageSize);
  f.headerStructBytes = sizeof(PoolHeader);
  f.ringControlBytes = sizeof(RingControl);
  f.recordBytes = sizeof(Record);
  f.poolRingLineBytes = sizeof(PoolRingLine);
  return f;
}

template <class F> constexpr bool matchesAfter(F tamper) {
  PoolFixedLine f = fixedFor(8, 1024, 4096);
  tamper(f);
  return detail::fixedLineMatches(f, 4096, f.totalBytes);
}

static_assert(matchesAfter([](PoolFixedLine &) {}));
static_assert(!matchesAfter([](PoolFixedLine &f) { f.version = 99; }));
static_assert(!matchesAfter([](PoolFixedLine &f) { ++f.recordBytes; }));
static_assert(!matchesAfter([](PoolFixedLine &f) { --f.headerStructBytes; }));
static_assert(!matchesAfter([](PoolFixedLine &f) { f.pageSize = 65536; }));
static_assert(!matchesAfter([](PoolFixedLine &f) { f.ringCount = 0; }));
static_assert(!matchesAfter([](PoolFixedLine &f) { f.ringCapacity = 1000; }));
static_assert(!matchesAfter([](PoolFixedLine &f) { f.ringStride += 4096; }));
static_assert(!matchesAfter([](PoolFixedLine &f) { f.headerBytes = 0; }));
static_assert(!matchesAfter([](PoolFixedLine &f) { ++f.ringCount; }));
// the recorded layout is self-consistent but the file is a page short
static_assert(!detail::fixedLineMatches(fixedFor(8, 1024, 4096), 4096,
                                        fixedFor(8, 1024, 4096).totalBytes -
                                            4096));

constexpr std::uint32_t testRingCount = 8;
constexpr std::uint64_t testCapacity = 1024;

// a pool laid out by hand in page-aligned heap memory, zeroed like a fresh
// memfd, with the fixed line filled as createPool will fill it.
class TestPool {
public:
  TestPool(std::uint32_t ringCount, std::uint64_t capacity,
           std::size_t pageSize)
      : layout(layoutFor(ringCount, capacity, pageSize)), align{pageSize},
        mem(::operator new(layout.totalBytes, align)) {
    std::memset(mem, 0, layout.totalBytes);
    auto *header = new (mem) PoolHeader{};
    PoolFixedLine &f = header->fixed;
    f.version = poolVersion;
    f.ringCount = ringCount;
    f.ringCapacity = capacity;
    f.headerBytes = layout.headerBytes;
    f.ringStride = layout.ringStride;
    f.totalBytes = layout.totalBytes;
    f.pageSize = static_cast<std::uint32_t>(pageSize);
    f.magic = poolMagic;
    pool = {header, static_cast<std::byte *>(mem), layout.totalBytes};
  }
  TestPool(const TestPool &) = delete;
  TestPool &operator=(const TestPool &) = delete;
  ~TestPool() { ::operator delete(mem, align); }

  PoolLayout layout;
  Pool pool;

private:
  std::align_val_t align;
  void *mem;
};

void checkPlacement(std::size_t pageSize) {
  TestPool t(testRingCount, testCapacity, pageSize);
  const Pool &pool = t.pool;

  bool aligned = true, adjacent = true, inside = true, free = true;
  for (std::uint32_t i = 0; i < pool.ringCount(); ++i) {
    auto *line = reinterpret_cast<std::byte *>(pool.ringLine(i));
    auto *ring = reinterpret_cast<std::byte *>(pool.ring(i));
    std::size_t offset = static_cast<std::size_t>(line - pool.base);
    aligned = aligned && offset % pageSize == 0 &&
              offset == t.layout.headerBytes + i * t.layout.ringStride;
    adjacent = adjacent && ring == line + sizeof(PoolRingLine);
    inside = inside &&
             ring + ringBytes(testCapacity) <= pool.base + t.layout.totalBytes;
    free = free && pool.ringLine(i)->state.load() == RingState::Free;
  }
  check(aligned, "place", "every ring line starts on a %zu B page boundary",
        pageSize);
  check(adjacent, "place", "each ring follows its %zu B pool line",
        sizeof(PoolRingLine));
  check(inside, "place", "the last ring ends inside the %zu B pool",
        t.layout.totalBytes);
  check(free, "place", "zero-filled memory reads as every ring Free");
  check(pool.liveness() == &pool.header->watcher.liveness, "place",
        "liveness() is the watcher line's");
}

// Write one record into every ring before draining any, so rings that
// overlapped would clobber each other.
void checkRoundTrip() {
  TestPool t(testRingCount, testCapacity, 4096);
  const Pool &pool = t.pool;
  for (std::uint32_t i = 0; i < pool.ringCount(); ++i)
    initRing(pool.ring(i), testCapacity);
  for (std::uint32_t i = 0; i < pool.ringCount(); ++i) {
    RingWriter w(pool.ring(i));
    for (std::uint64_t k = 0; k < testCapacity; ++k) {
      std::uint64_t s = w.claim(1);
      w.slot(s) = Record{};
      w.slot(s).kind = Kind::Alloc;
      w.slot(s).addr = i;
      w.slot(s).size = k;
      w.publish(s, 1);
    }
  }
  std::uint32_t intact = 0;
  for (std::uint32_t i = 0; i < pool.ringCount(); ++i) {
    RingView v(pool.ring(i));
    bool ok = v.pending() == testCapacity;
    std::uint64_t k = 0;
    v.drain([&](std::span<const Record> a, std::span<const Record> b,
                std::uint64_t) {
      for (std::span<const Record> part : {a, b})
        for (const Record &r : part)
          ok = ok && r.addr == i && r.size == k++;
    });
    intact += ok && k == testCapacity;
  }
  check(intact == testRingCount, "trip",
        "%u of %u full rings drain intact after all were filled", intact,
        testRingCount);
}

// A createPool result that is destroyed on every path out of a check.
class LivePool {
public:
  explicit LivePool(const PoolConfig &config) : result(createPool(config)) {
    if (!result)
      check(false, "create", "createPool failed at %s: %s", result.error().step,
            std::strerror(result.error().error));
  }
  LivePool(const LivePool &) = delete;
  LivePool &operator=(const LivePool &) = delete;
  ~LivePool() {
    if (result)
      destroyPool(*result);
  }

  explicit operator bool() const { return result.has_value(); }
  CreatedPool &operator*() { return *result; }
  CreatedPool *operator->() { return &*result; }

private:
  std::expected<CreatedPool, PoolError> result;
};

constexpr PoolConfig testConfig{testRingCount, testCapacity,
                                defaultHeartbeatTimeoutNs};
constexpr std::int64_t secondNs = 1'000'000'000;

void checkCreate() {
  LivePool created(testConfig);
  if (!created)
    return;
  const Pool &pool = created->pool;
  const PoolFixedLine &f = pool.header->fixed;
  PoolLayout layout = layoutFor(testRingCount, testCapacity, f.pageSize);
  auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));

  check(std::atomic_ref(pool.header->fixed.magic)
                    .load(std::memory_order_acquire) == poolMagic &&
            f.version == poolVersion,
        "create", "magic and version are set");
  check(f.ringCount == testRingCount && f.ringCapacity == testCapacity &&
            f.pageSize == page && f.headerBytes == layout.headerBytes &&
            f.ringStride == layout.ringStride &&
            f.totalBytes == layout.totalBytes &&
            pool.bytes == layout.totalBytes &&
            reinterpret_cast<std::byte *>(pool.header) == pool.base,
        "create", "the recorded layout matches layoutFor at %zu B pages", page);
  check(f.headerStructBytes == sizeof(PoolHeader) &&
            f.ringControlBytes == sizeof(RingControl) &&
            f.recordBytes == sizeof(Record) &&
            f.poolRingLineBytes == sizeof(PoolRingLine),
        "create", "the build-mismatch sizes match this build");
  check(f.watcherPid == static_cast<std::uint32_t>(::getpid()), "create",
        "watcherPid is the creator's pid");
  const PoolLiveness &live = *pool.liveness();
  auto age =
      ta::monotonicNs() - static_cast<std::int64_t>(live.heartbeatNs.load());
  check(age >= 0 && age < secondNs && live.stopping.load() == 0 &&
            live.timeoutNs == defaultHeartbeatTimeoutNs,
        "create", "heartbeat is fresh (%lld ns old), not stopping, timeout set",
        static_cast<long long>(age));

  int fd = created->fd;
  struct stat st{};
  int seals = ::fcntl(fd, F_GET_SEALS);
  constexpr int wanted = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
  check(::fstat(fd, &st) == 0 &&
            static_cast<std::size_t>(st.st_size) == layout.totalBytes,
        "fd", "memfd size is totalBytes");
  check(seals >= 0 && (seals & wanted) == wanted, "fd",
        "sealed against shrink, grow and new seals (0x%x)", seals);
  errno = 0;
  bool refused = ::ftruncate(fd, 0) != 0 && errno == EPERM;
  check(refused, "fd", "ftruncate on the pool fails with EPERM");
  check((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "fd",
        "FD_CLOEXEC is set; the launcher clears it only in the child");

  bool rings = true;
  for (std::uint32_t i = 0; i < pool.ringCount(); ++i) {
    const PoolRingLine &line = *pool.ringLine(i);
    const RingControl &ring = *pool.ring(i);
    rings = rings && line.state.load() == RingState::Free && line.tid == 0 &&
            line.serial == 0 && ring.fixed.magic == ringMagic &&
            ring.fixed.capacity == testCapacity &&
            RingView(pool.ring(i)).pending() == 0;
  }
  check(rings, "create", "all %u rings are Free, initialised and empty",
        pool.ringCount());
}

// Only offsets are shared: a writer on one mapping and a view on another,
// at a different address, see the same ring.
void checkTwoMappings() {
  LivePool created(testConfig);
  if (!created)
    return;
  const Pool &a = created->pool;
  void *mem = ::mmap(nullptr, a.bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                     created->fd, 0);
  if (mem == MAP_FAILED) {
    check(false, "mappings", "second mmap failed: %s", std::strerror(errno));
    return;
  }
  Pool b{static_cast<PoolHeader *>(mem), static_cast<std::byte *>(mem),
         a.bytes};
  check(b.base != a.base &&
            b.header->fixed.ringStride == a.header->fixed.ringStride,
        "mappings", "a second mapping at another address sees the header");

  constexpr std::uint64_t total = 100'000;
  constexpr std::uint64_t batch = testCapacity / 2;
  RingWriter w(a.ring(3));
  RingView v(b.ring(3));
  std::uint64_t written = 0, expected = 0, bad = 0;
  while (written < total) {
    for (std::uint64_t k = 0; k < batch && written < total; ++k, ++written) {
      std::uint64_t s = w.claim(1);
      w.slot(s) = Record{};
      w.slot(s).kind = Kind::Alloc;
      w.slot(s).addr = s;
      w.publish(s, 1);
    }
    v.drain([&](std::span<const Record> x, std::span<const Record> y,
                std::uint64_t) {
      for (std::span<const Record> part : {x, y})
        for (const Record &r : part)
          bad += r.addr != expected++;
    });
  }
  check(expected == total && bad == 0, "mappings",
        "%llu records written through one mapping read back through the "
        "other, %llu out of place",
        static_cast<unsigned long long>(expected),
        static_cast<unsigned long long>(bad));
  ::munmap(mem, a.bytes);
}

void checkBadConfig() {
  constexpr PoolConfig bad[] = {
      {0, testCapacity, defaultHeartbeatTimeoutNs},
      {maxRingCount + 1, testCapacity, defaultHeartbeatTimeoutNs},
      {testRingCount, 1000, defaultHeartbeatTimeoutNs},
      {testRingCount, maxRingCapacity * 2, defaultHeartbeatTimeoutNs},
      {testRingCount, testCapacity, 0},
  };
  std::size_t rejected = 0;
  for (const PoolConfig &config : bad) {
    auto result = createPool(config);
    if (!result && std::strcmp(result.error().step, "config") == 0 &&
        result.error().error == EINVAL)
      ++rejected;
    else if (result)
      destroyPool(*result);
  }
  check(rejected == std::size(bad), "config",
        "%zu of %zu bad configs return {config, EINVAL} without aborting",
        rejected, std::size(bad));
}

void checkDestroy() {
  auto result = createPool(testConfig);
  if (!result) {
    check(false, "destroy", "createPool failed at %s", result.error().step);
    return;
  }
  CreatedPool &created = *result;
  int fd = created.fd;
  destroyPool(created);
  errno = 0;
  bool closed = ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
  check(closed && created.fd == -1 && created.pool.base == nullptr &&
            created.pool.header == nullptr && created.pool.bytes == 0,
        "destroy", "destroyPool closes the fd and resets the handle");
  destroyPool(created);
  check(true, "destroy", "a second destroyPool does nothing");
}

void checkHelpers() {
  LivePool created(testConfig);
  if (!created)
    return;
  const Pool &pool = created->pool;
  check(attachedPid(pool) == 0 && disabledThreads(pool) == 0, "helpers",
        "attachedPid and disabledThreads start at 0");

  std::uint64_t before = pool.liveness()->heartbeatNs.load();
  std::this_thread::sleep_for(std::chrono::milliseconds{1});
  beat(pool);
  std::uint64_t after = pool.liveness()->heartbeatNs.load();
  check(after > before, "helpers", "beat moves the heartbeat forward %llu ns",
        static_cast<unsigned long long>(after - before));

  stop(pool);
  check(pool.liveness()->stopping.load() == 1, "helpers", "stop sets stopping");
}

// attachPool's result as a child's exit code: 0 attached, 10 + the error
// otherwise, 99 when a failed attach closed the fd it doesn't own. a signal
// shows as 128 + its number (134 abort, 135 SIGBUS).
constexpr int attachedCode = 0;
constexpr int fdClosedCode = 99;

constexpr int codeFor(AttachError error) {
  return 10 + static_cast<int>(error);
}

int failedAttachCode(int fd) {
  auto attached = attachPool(fd);
  if (attached)
    return attachedCode;
  return ::fcntl(fd, F_GETFD) == -1 ? fdClosedCode : codeFor(attached.error());
}

// runs f in a forked child and returns its exit code, so a crash or a
// contract abort in attachPool fails one check instead of the whole run.
template <class F> int exitCodeInChild(F f) {
  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
    ::_exit(f());
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return WEXITSTATUS(status);
}

int sealedMemfd(std::size_t bytes) {
  int fd = ::memfd_create("pool-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0)
    return -1;
  if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0 ||
      ::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Everything here is turned away before the parent check, so it runs in
// this process; the fd must stay open every time.
void checkAttachRejects() {
  int closed = ::memfd_create("pool-test", MFD_CLOEXEC);
  ::close(closed);
  // never open, so the fd-stays-open check in failedAttachCode doesn't apply
  auto isNoPool = [](int fd) {
    auto attached = attachPool(fd);
    return !attached && attached.error() == AttachError::NoPool;
  };
  check(isNoPool(-1) && isNoPool(closed), "attach",
        "fd -1 and a closed fd are NoPool");

  auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  LivePool created(testConfig);
  if (!created)
    return;
  const Pool &pool = created->pool;

  // a byte-for-byte copy of a real pool's header, minus the seals
  int unsealed = ::memfd_create("pool-test", MFD_CLOEXEC);
  bool copied =
      unsealed >= 0 &&
      ::ftruncate(unsealed, static_cast<off_t>(pool.bytes)) == 0 &&
      ::pwrite(unsealed, pool.base, page, 0) == static_cast<ssize_t>(page);
  check(copied && failedAttachCode(unsealed) == codeFor(AttachError::BadPool),
        "attach", "an unsealed copy of a valid header is BadPool");
  ::close(unsealed);

  int devNull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
  check(failedAttachCode(devNull) == codeFor(AttachError::BadPool), "attach",
        "a file that can't carry seals (/dev/null) is BadPool");
  ::close(devNull);

  int zeros = sealedMemfd(2 * page);
  check(zeros >= 0 && failedAttachCode(zeros) == codeFor(AttachError::BadPool),
        "attach", "a sealed memfd of zeros is BadPool (bad magic)");
  ::close(zeros);

  // an empty file would raise SIGBUS on the magic read without the size
  // check; run in a child so a signal fails the check, not the run
  int empty = sealedMemfd(0);
  int tiny = sealedMemfd(64);
  int emptyCode = exitCodeInChild([empty] { return failedAttachCode(empty); });
  int tinyCode = exitCodeInChild([tiny] { return failedAttachCode(tiny); });
  check(emptyCode == codeFor(AttachError::BadPool) &&
            tinyCode == codeFor(AttachError::BadPool),
        "attach", "sealed memfds of 0 and 64 B are BadPool (codes %d, %d)",
        emptyCode, tinyCode);
  ::close(empty);
  ::close(tiny);

  check(failedAttachCode(created->fd) == codeFor(AttachError::NotOurWatcher) &&
            attachedPid(pool) == 0,
        "attach",
        "the watcher attaching to its own pool is NotOurWatcher and claims "
        "nothing");
}

// A field changed through the watcher's mapping, attached from a child: its
// parent is the watcher, so only the validation can turn it away. 0 rings
// and capacity 1000 must come back BadPool, not abort on layoutFor's
// contract.
void checkAttachTampered() {
  LivePool created(testConfig);
  if (!created)
    return;
  PoolFixedLine &f = created->pool.header->fixed;
  int fd = created->fd;
  auto tampered = [&](auto change) {
    PoolFixedLine saved = f;
    change(f);
    int code = exitCodeInChild([fd] { return failedAttachCode(fd); });
    f = saved;
    return code;
  };

  struct Case {
    const char *what;
    int code;
  };
  const Case cases[] = {
      {"magic", tampered([](PoolFixedLine &l) { l.magic ^= 1; })},
      {"version 99", tampered([](PoolFixedLine &l) { l.version = 99; })},
      {"recordBytes + 1", tampered([](PoolFixedLine &l) { ++l.recordBytes; })},
      {"totalBytes - 1 page",
       tampered([](PoolFixedLine &l) { l.totalBytes -= l.pageSize; })},
      {"ringCount 0", tampered([](PoolFixedLine &l) { l.ringCount = 0; })},
      {"capacity 1000",
       tampered([](PoolFixedLine &l) { l.ringCapacity = 1000; })},
  };
  for (const Case &c : cases)
    check(c.code == codeFor(AttachError::BadPool), "tamper",
          "%s is BadPool in the target (code %d)", c.what, c.code);
  check(attachedPid(created->pool) == 0, "tamper",
        "no tampered attach claimed the pool");
}

constexpr std::uint64_t attachRecords = 3;

// The target's side: attach, check what it sees, write into ring 0.
int attachAndWrite(int fd) {
  auto attached = attachPool(fd);
  if (!attached)
    return codeFor(attached.error());
  if (::fcntl(fd, F_GETFD) != -1)
    return 1; // success must close the fd
  const Pool &pool = *attached;
  if (pool.ringCount() != testRingCount ||
      pool.bytes != pool.header->fixed.totalBytes)
    return 2;
  for (std::uint32_t i = 0; i < pool.ringCount(); ++i)
    if (pool.ringLine(i)->state.load() != RingState::Free)
      return 3;
  RingWriter w(pool.ring(0));
  for (std::uint64_t k = 0; k < attachRecords; ++k) {
    std::uint64_t s = w.claim(1);
    w.slot(s) = Record{};
    w.slot(s).kind = Kind::Alloc;
    w.slot(s).addr = 0xa000 + k;
    w.publish(s, 1);
  }
  return attachedCode;
}

void checkAttach() {
  LivePool created(testConfig);
  if (!created)
    return;
  const Pool &pool = created->pool;
  int fd = created->fd;

  std::fflush(nullptr);
  pid_t child = ::fork();
  if (child == 0)
    ::_exit(attachAndWrite(fd));
  int status = 0;
  ::waitpid(child, &status, 0);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  check(code == attachedCode, "attach",
        "a child attaches, sees %u Free rings and has its fd closed (code %d)",
        testRingCount, code);
  check(attachedPid(pool) == static_cast<std::uint32_t>(child), "attach",
        "attachedPid is the child's pid");
  RingView v(pool.ring(0));
  std::uint64_t seen = 0, intact = 0;
  v.drain(
      [&](std::span<const Record> a, std::span<const Record> b, std::uint64_t) {
        for (std::span<const Record> part : {a, b})
          for (const Record &r : part)
            intact += r.addr == 0xa000 + seen++;
      });
  check(seen == attachRecords && intact == attachRecords, "attach",
        "the watcher reads the child's %llu records through its own mapping",
        static_cast<unsigned long long>(attachRecords));

  int sibling = exitCodeInChild([fd] { return failedAttachCode(fd); });
  check(sibling == codeFor(AttachError::AlreadyAttached) &&
            attachedPid(pool) == static_cast<std::uint32_t>(child),
        "attach", "a sibling after it is AlreadyAttached (code %d)", sibling);
}

void checkAttachTwice() {
  LivePool created(testConfig);
  if (!created)
    return;
  int fd = created->fd;
  // the first success closes fd, so the second attach uses a dup of it
  int code = exitCodeInChild([fd] {
    int again = ::dup(fd);
    if (!attachPool(fd))
      return 1;
    return failedAttachCode(again);
  });
  check(code == codeFor(AttachError::AlreadyAttached), "attach",
        "one process attaching twice is AlreadyAttached (code %d)", code);
}

// TATRACE_FD reaches grandchildren through the environment; the parent
// check is what keeps them off the pool.
void checkAttachGrandchild() {
  LivePool created(testConfig);
  if (!created)
    return;
  int fd = created->fd;
  int code = exitCodeInChild(
      [fd] { return exitCodeInChild([fd] { return failedAttachCode(fd); }); });
  check(code == codeFor(AttachError::NotOurWatcher) &&
            attachedPid(created->pool) == 0,
        "attach",
        "a grandchild is NotOurWatcher, keeps its fd and claims nothing "
        "(code %d)",
        code);
}

// An address-space limit just above what the child already maps, so only
// the phase-2 map fails. Sanitizers reserve address space of their own, so
// the limit can't be set reliably there.
void checkAttachMapFailed() {
#if defined(TA_SANITIZED)
  skip("attach", "MapFailed needs an exact RLIMIT_AS; not under sanitizers");
#else
  constexpr PoolConfig big{32, defaultRingCapacity, defaultHeartbeatTimeoutNs};
  LivePool created(big);
  if (!created)
    return;
  int fd = created->fd;
  int code = exitCodeInChild([fd] {
    // what this process maps now (the watcher's pool included), plus 16 MiB:
    // room for phase 1's page, none for phase 2's 80 MiB
    std::FILE *statm = std::fopen("/proc/self/statm", "r");
    unsigned long pages = 0;
    if (!statm || std::fscanf(statm, "%lu", &pages) != 1)
      return 1;
    std::fclose(statm);
    auto page = static_cast<rlim_t>(::sysconf(_SC_PAGESIZE));
    rlim_t limit = pages * page + (rlim_t{16} << 20);
    rlimit r{limit, limit};
    if (::setrlimit(RLIMIT_AS, &r) != 0)
      return 2;
    auto attached = attachPool(fd);
    if (attached)
      return attachedCode;
    if (attached.error() == AttachError::MapFailed && errno != ENOMEM)
      return 3;
    return codeFor(attached.error());
  });
  check(code == codeFor(AttachError::MapFailed), "attach",
        "with no address space for the pool, attach is MapFailed with ENOMEM "
        "(code %d)",
        code);
  check(attachedPid(created->pool) == 0, "attach",
        "a failed map claims nothing");
#endif
}

constexpr std::uint64_t shortTimeoutNs = 50'000'000; // 50 ms

std::uint64_t nowNs() { return static_cast<std::uint64_t>(ta::monotonicNs()); }

void checkWatcherGone() {
  PoolLiveness live{};
  live.timeoutNs = shortTimeoutNs;
  auto gone = [&] { return detail::watcherGone(&live); };

  live.heartbeatNs.store(nowNs());
  check(!gone(), "giveup", "a fresh heartbeat is a live watcher");
  live.heartbeatNs.store(nowNs() - 2 * shortTimeoutNs);
  check(gone(), "giveup", "a heartbeat older than the timeout is gone");
  // the watcher can beat between the producer's two reads; unsigned
  // now - heartbeat would wrap to a huge age
  live.heartbeatNs.store(nowNs() + secondNs);
  check(!gone(), "giveup", "a heartbeat from the future is a live watcher");
  live.heartbeatNs.store(nowNs());
  live.stopping.store(1);
  check(gone(), "giveup", "stopping is gone, whatever the heartbeat");
}

void fillRing(RingWriter &w, std::uint64_t count) {
  for (std::uint64_t k = 0; k < count; ++k) {
    std::uint64_t s = w.claim(1);
    w.slot(s) = Record{};
    w.slot(s).kind = Kind::Alloc;
    w.slot(s).addr = s;
    w.publish(s, 1);
  }
}

void checkGiveUpWriter() {
  LivePool created({testRingCount, testCapacity, shortTimeoutNs});
  if (!created)
    return;
  const Pool &pool = created->pool;

  // stale: the watcher stopped beating a second ago
  RingWriter stale(pool.ring(0), poolGiveUp(pool));
  fillRing(stale, testCapacity);
  pool.liveness()->heartbeatNs.store(nowNs() - secondNs);
  std::uint64_t s = stale.claim(1);
  check(s == claimFailed && RingView(pool.ring(0)).counters().dropped == 1,
        "giveup", "a full ring with a stale heartbeat returns claimFailed");

  // fresh: a watcher thread keeps beating and drains after 20 ms, which is
  // past the spin phase but inside the 50 ms timeout
  beat(pool);
  RingWriter live(pool.ring(1), poolGiveUp(pool));
  fillRing(live, testCapacity);
  std::atomic<bool> done = false;
  std::thread watcher([&] {
    auto start = std::chrono::steady_clock::now();
    bool drained = false;
    while (!done.load()) {
      beat(pool);
      if (!drained && std::chrono::steady_clock::now() - start >
                          std::chrono::milliseconds{20})
        drained = RingView(pool.ring(1)).drain([](auto, auto, auto) {}) > 0;
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  });
  s = live.claim(1);
  done.store(true);
  watcher.join();
  RingCounters c = RingView(pool.ring(1)).counters();
  check(s == testCapacity && c.dropped == 0 && c.stallCount == 1, "giveup",
        "with a beating watcher, a full ring blocks and then claims");
}

void checkClaimSequential() {
  LivePool created(testConfig);
  if (!created)
    return;
  const Pool &pool = created->pool;
  std::uint32_t indices = 0, serials = 0;
  bool recorded = true;
  for (std::uint32_t k = 0; k < testRingCount; ++k) {
    auto claimed = claimRing(pool, 100 + k);
    if (!claimed) {
      recorded = false;
      break;
    }
    const PoolRingLine &line = *pool.ringLine(claimed->index);
    indices |= 1u << claimed->index;
    serials |= 1u << line.serial;
    recorded = recorded && claimed->ring == pool.ring(claimed->index) &&
               ringState(pool, claimed->index) == RingState::Active &&
               line.tid == 100 + k;
  }
  constexpr std::uint32_t all = (1u << testRingCount) - 1;
  check(recorded && indices == all && serials == all, "claim",
        "%u claims take every ring once, Active, with serials 0..%u and the "
        "caller's tid",
        testRingCount, testRingCount - 1);
  check(!claimRing(pool, 999) && disabledThreads(pool) == 1, "claim",
        "with every ring taken, a claim is nullopt and counts a disabled "
        "thread");
}

void checkClaimConcurrent() {
  LivePool created(testConfig);
  if (!created)
    return;
  const Pool &pool = created->pool;
  constexpr std::uint32_t threads = testRingCount + 4;
  std::optional<ClaimedRing> results[threads];
  std::uint32_t serialOf[threads] = {};
  std::atomic<bool> go = false;
  std::vector<std::thread> workers;
  for (std::uint32_t t = 0; t < threads; ++t)
    workers.emplace_back([&, t] {
      while (!go.load())
        ;
      results[t] = claimRing(pool, 1000 + t);
      if (results[t])
        serialOf[t] = pool.ringLine(results[t]->index)->serial;
    });
  go.store(true);
  for (std::thread &t : workers)
    t.join();

  std::uint32_t won = 0, indices = 0, serials = 0;
  for (std::uint32_t t = 0; t < threads; ++t)
    if (results[t]) {
      ++won;
      indices |= 1u << results[t]->index;
      serials |= 1u << serialOf[t];
    }
  constexpr std::uint32_t all = (1u << testRingCount) - 1;
  check(won == testRingCount && indices == all && serials == all &&
            disabledThreads(pool) == threads - testRingCount,
        "claim",
        "%u threads racing for %u rings: %u win distinct rings and serials, "
        "%llu disabled",
        threads, testRingCount, won,
        static_cast<unsigned long long>(disabledThreads(pool)));
}

void checkCloseRecycle() {
  LivePool created({1, testCapacity, defaultHeartbeatTimeoutNs});
  if (!created)
    return;
  const Pool &pool = created->pool;
  auto claimed = claimRing(pool, 42);
  if (!claimed) {
    check(false, "recycle", "claim of the only ring failed");
    return;
  }
  RingWriter w(claimed->ring);
  fillRing(w, 5);
  closeRing(pool, claimed->index);
  check(ringState(pool, 0) == RingState::Closed, "recycle",
        "closeRing leaves the ring Closed");

  RingView v(claimed->ring);
  std::uint64_t drained = v.drain([](auto, auto, auto) {});
  RingStats st = ringStats(pool, 0);
  check(drained == 5 && st.tid == 42 && st.serial == 0 && st.events == 5 &&
            st.dropped == 0,
        "recycle", "ringStats reports tid %u, serial %u and %llu events",
        st.tid, st.serial, static_cast<unsigned long long>(st.events));

  recycleRing(pool, 0);
  const PoolRingLine &line = *pool.ringLine(0);
  RingCounters c = v.counters();
  check(ringState(pool, 0) == RingState::Free && line.tid == 0 &&
            line.serial == 0 && c.events == 0 && v.pending() == 0,
        "recycle", "recycleRing leaves it Free, empty, with no owner");

  auto again = claimRing(pool, 43);
  bool fresh = again && again->index == 0 && line.serial == 1 &&
               line.tid == 43 && RingWriter(again->ring).claim(1) == 0;
  check(fresh, "recycle",
        "the next claim gets it back with a new serial, from seq 0");
}

constexpr std::uint64_t childRecords = 1000;

// The target's side: attach, claim with the real tid, write, close.
int claimWriteClose(int fd) {
  auto attached = attachPool(fd);
  if (!attached)
    return codeFor(attached.error());
  auto claimed = claimRing(*attached, static_cast<std::uint32_t>(::gettid()));
  if (!claimed)
    return 1;
  RingWriter w(claimed->ring, poolGiveUp(*attached));
  fillRing(w, childRecords);
  closeRing(*attached, claimed->index);
  return 0;
}

void checkClaimCrossProcess() {
  LivePool created(testConfig);
  if (!created)
    return;
  const Pool &pool = created->pool;
  int fd = created->fd;

  std::fflush(nullptr);
  pid_t child = ::fork();
  if (child == 0)
    ::_exit(claimWriteClose(fd));
  int status = 0;
  ::waitpid(child, &status, 0);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

  std::uint32_t closed = pool.ringCount();
  for (std::uint32_t i = 0; i < pool.ringCount(); ++i)
    if (ringState(pool, i) == RingState::Closed)
      closed = i;
  check(code == 0 && closed < pool.ringCount(), "process",
        "a child attaches, claims, writes and closes a ring (code %d)", code);
  if (closed == pool.ringCount())
    return;

  std::uint64_t expected = 0, bad = 0;
  RingView(pool.ring(closed))
      .drain([&](std::span<const Record> a, std::span<const Record> b,
                 std::uint64_t) {
        for (std::span<const Record> part : {a, b})
          for (const Record &r : part)
            bad += r.addr != expected++;
      });
  RingStats st = ringStats(pool, closed);
  // the child's only thread is its main thread, whose tid is its pid
  check(expected == childRecords && bad == 0 &&
            st.tid == static_cast<std::uint32_t>(child) &&
            st.events == childRecords,
        "process",
        "the watcher drains all %llu records in order, with the child's tid",
        static_cast<unsigned long long>(childRecords));
  recycleRing(pool, closed);
  check(ringState(pool, closed) == RingState::Free, "process",
        "and recycles the ring");
}

void checkLifecycleContracts() {
  if (!contractsAbort()) {
    skip("contract", "contracts not enforced in this build");
    return;
  }
  LivePool created({3, testCapacity, defaultHeartbeatTimeoutNs});
  if (!created)
    return;
  Pool pool = created->pool;
  // ring 0 Active; ring 1 Closed with records left; ring 2 Free
  auto active = claimRing(pool, 1);
  auto pending = claimRing(pool, 2);
  if (!active || !pending) {
    check(false, "contract", "setup claims failed");
    return;
  }
  RingWriter w(pending->ring);
  fillRing(w, 3);
  closeRing(pool, pending->index);
  std::uint32_t freeIndex = 3 - active->index - pending->index;

  check(abortsInChild([pool, freeIndex] { closeRing(pool, freeIndex); }),
        "contract", "closeRing on a Free ring aborts");
  check(abortsInChild([pool, i = active->index] { recycleRing(pool, i); }),
        "contract", "recycleRing on an Active ring aborts");
  check(abortsInChild([pool, i = pending->index] { recycleRing(pool, i); }),
        "contract", "recycleRing before the final drain aborts");
}

// Volatile, so the calls happen at run time where contracts are checked.
volatile std::uint32_t runtimeZeroRings = 0;
volatile std::uint64_t runtimeOddCapacity = 1000;
volatile std::uint64_t runtimeHugeCapacity = maxRingCapacity * 2;
volatile std::size_t runtimeTinyPage = 64;

void checkParseFd() {
  bool good = parsePoolFd("0") == 0 && parsePoolFd("5") == 5 &&
              parsePoolFd("2147483647") == 2147483647;
  check(good, "parse", "plain digits parse, up to INT_MAX");
  const char *bad[] = {"",   "abc", "-1",  "+5", " 3",         "3 ",
                       "3x", "0x5", "1e3", "-0", "2147483648", "99999999999"};
  std::size_t rejected = 0;
  for (const char *value : bad)
    rejected += parsePoolFd(value) == -1;
  check(parsePoolFd(nullptr) == -1 && rejected == std::size(bad), "parse",
        "unset and %zu of %zu malformed values give -1", rejected,
        std::size(bad));
}

void checkContracts() {
  if (!contractsAbort()) {
    skip("contract", "contracts not enforced in this build");
    return;
  }
  check(abortsInChild([] {
          static_cast<void>(layoutFor(runtimeZeroRings, testCapacity, 4096));
        }),
        "contract", "layoutFor with no rings aborts");
  check(abortsInChild(
            [] { static_cast<void>(layoutFor(1, runtimeOddCapacity, 4096)); }),
        "contract",
        "layoutFor with a capacity that isn't a power of two "
        "aborts");
  check(abortsInChild(
            [] { static_cast<void>(layoutFor(1, runtimeHugeCapacity, 4096)); }),
        "contract", "layoutFor above maxRingCapacity aborts");
  check(abortsInChild([] {
          static_cast<void>(layoutFor(1, testCapacity, runtimeTinyPage));
        }),
        "contract", "layoutFor with a page smaller than a ring line aborts");

  TestPool t(testRingCount, testCapacity, 4096);
  Pool pool = t.pool;
  check(
      abortsInChild([pool] { static_cast<void>(pool.ring(pool.ringCount())); }),
      "contract", "ring(ringCount()) aborts");
  check(abortsInChild(
            [pool] { static_cast<void>(pool.ringLine(pool.ringCount())); }),
        "contract", "ringLine(ringCount()) aborts");
}

} // namespace

int main() {
  checkPlacement(4096);
  auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  if (page != 4096)
    checkPlacement(page);
  checkRoundTrip();
  checkCreate();
  checkTwoMappings();
  checkBadConfig();
  checkDestroy();
  checkHelpers();
  checkParseFd();
  checkAttachRejects();
  checkAttachTampered();
  checkAttach();
  checkAttachTwice();
  checkAttachGrandchild();
  checkAttachMapFailed();
  checkWatcherGone();
  checkGiveUpWriter();
  checkClaimSequential();
  checkClaimConcurrent();
  checkCloseRecycle();
  checkClaimCrossProcess();
  checkLifecycleContracts();
  checkContracts();
  return finish();
}
