// Checks for the pass-through shim (libtieralloc-trace.so). Run it with the
// shim first in LD_PRELOAD, optionally followed by another allocator; it fails
// at once if the shim isn't loaded. Checks that all 33 entry points are
// interposed, that the C and C++ entry points keep their contracts (alignment,
// errno, return codes, zeroing, contents across realloc), that running out of
// memory ends exactly as it does without the shim, and that threads
// allocating and freeing each other's blocks work. Exits nonzero if any check
// fails.
//
// The out-of-memory checks re-run this binary as a child, once with the shim
// and once with it removed from LD_PRELOAD, and compare how each ends:
// allocators differ (glibc and jemalloc throw bad_alloc; mimalloc and
// tcmalloc abort), and the shim must not change which.

#include "ta/testing.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

using namespace ta::test;

// The test's own list, so a typo in the shim's lookup table can't also be
// copied here.
constexpr std::array<const char *, 33> hookNames = {
    "malloc",
    "free",
    "calloc",
    "realloc",
    "reallocarray",
    "memalign",
    "posix_memalign",
    "aligned_alloc",
    "valloc",
    "pvalloc",
    "malloc_usable_size",
    "free_sized",
    "free_aligned_sized",
    "_Znwm",
    "_Znam",
    "_ZnwmRKSt9nothrow_t",
    "_ZnamRKSt9nothrow_t",
    "_ZnwmSt11align_val_t",
    "_ZnamSt11align_val_t",
    "_ZnwmSt11align_val_tRKSt9nothrow_t",
    "_ZnamSt11align_val_tRKSt9nothrow_t",
    "_ZdlPv",
    "_ZdaPv",
    "_ZdlPvRKSt9nothrow_t",
    "_ZdaPvRKSt9nothrow_t",
    "_ZdlPvm",
    "_ZdaPvm",
    "_ZdlPvSt11align_val_t",
    "_ZdaPvSt11align_val_t",
    "_ZdlPvmSt11align_val_t",
    "_ZdaPvmSt11align_val_t",
    "_ZdlPvSt11align_val_tRKSt9nothrow_t",
    "_ZdaPvSt11align_val_tRKSt9nothrow_t",
};

constexpr std::string_view shimName = "libtieralloc-trace";
constexpr std::size_t pageBytes = 4096;
constexpr int formRounds = 10'000;
constexpr int threadCount = 8;
constexpr int opsPerThread = 200'000;
constexpr std::size_t handoffBatch = 64;

// Volatile, so the compiler can't fold or drop the huge requests.
volatile std::size_t huge = std::numeric_limits<std::size_t>::max() / 2;

// Makes p escape, so the compiler can't elide an allocation and its free; the
// point is that every call reaches the shim.
void keep(const void *p) { asm volatile("" : : "r"(p) : "memory"); }

// Hides p's identity from the compiler, which otherwise assumes a pointer
// passed to realloc is dead even when the call fails and leaves it valid.
void *opaque(void *p) {
  asm volatile("" : "+r"(p));
  return p;
}

bool aligned(const void *p, std::size_t align) {
  return reinterpret_cast<std::uintptr_t>(p) % align == 0;
}

const void *objectBase(const void *symbol) {
  Dl_info info;
  return dladdr(symbol, &info) ? info.dli_fbase : nullptr;
}

const void *shimBase() {
  void *probe = dlsym(RTLD_DEFAULT, "tatrace_max_depth");
  return probe ? objectBase(probe) : nullptr;
}

bool filled(const void *p, std::size_t bytes, unsigned char value) {
  auto *bytesIn = static_cast<const unsigned char *>(p);
  for (std::size_t i = 0; i < bytes; ++i)
    if (bytesIn[i] != value)
      return false;
  return true;
}

// LD_PRELOAD's entries; ld.so accepts spaces and colons between them.
std::vector<std::string> preloadEntries(std::string_view preload) {
  std::vector<std::string> entries;
  while (!preload.empty()) {
    std::size_t end = preload.find_first_of(" :");
    if (end != 0)
      entries.emplace_back(preload.substr(0, end));
    preload = end == std::string_view::npos ? "" : preload.substr(end + 1);
  }
  return entries;
}

// A wrong path in LD_PRELOAD only makes ld.so print a warning, so a run meant
// for jemalloc would quietly test glibc. Every entry must really be loaded.
void checkPreloaded() {
  const char *preload = std::getenv("LD_PRELOAD");
  std::vector<std::string> entries = preloadEntries(preload ? preload : "");
  std::string missing;
  for (const std::string &entry : entries) {
    if (void *handle = dlopen(entry.c_str(), RTLD_LAZY | RTLD_NOLOAD)) {
      dlclose(handle);
      continue;
    }
    missing += missing.empty() ? " (" : ", ";
    missing += entry;
  }
  if (!missing.empty())
    missing += ")";
  check(missing.empty(), "preload", "all %zu LD_PRELOAD entries are loaded%s",
        entries.size(), missing.c_str());
}

// Every name, looked up the way ld.so binds callers, must land in the shim.
// An allocator listed before the shim in LD_PRELOAD fails this.
void checkInterposed(const void *base) {
  int missing = 0;
  std::string names;
  for (const char *name : hookNames) {
    void *symbol = dlsym(RTLD_DEFAULT, name);
    if (symbol && objectBase(symbol) == base)
      continue;
    ++missing;
    names += names.empty() ? " (" : ", ";
    names += name;
  }
  if (!names.empty())
    names += ")";
  check(missing == 0, "interpose",
        "%zu of %zu entry points resolve to the shim%s",
        hookNames.size() - missing, hookNames.size(), names.c_str());
}

// C only requires alignment for any object that fits: an 8 B block may be
// 8-aligned (mimalloc does this), even though glibc always gives 16.
std::size_t requiredAlign(std::size_t bytes) {
  return std::min(alignof(std::max_align_t), std::bit_floor(bytes));
}

void checkMalloc() {
  bool ok = true;
  for (std::size_t bytes : {1uz, 8uz, 24uz, 100uz, 4096uz, 1uz << 20}) {
    void *p = std::malloc(bytes);
    keep(p);
    ok = ok && p && aligned(p, requiredAlign(bytes)) &&
         malloc_usable_size(p) >= bytes;
    if (p)
      std::memset(p, 0xab, bytes);
    std::free(p);
  }
  check(ok, "malloc",
        "1 B to 1 MiB: non-null, aligned for its size, usable size covers it");
  check(malloc_usable_size(nullptr) == 0, "usable",
        "malloc_usable_size(nullptr) is 0");

  errno = 0;
  void *p = std::malloc(huge);
  keep(p);
  check(p == nullptr && errno == ENOMEM, "malloc",
        "huge request: null with ENOMEM (errno %d)", errno);
}

void checkCalloc() {
  constexpr std::size_t bytes = 1000;
  // Dirty a block of the same size first, so a reused block shows up if it
  // isn't zeroed.
  void *dirty = std::malloc(bytes);
  keep(dirty);
  std::memset(dirty, 0xcd, bytes);
  std::free(dirty);
  void *p = std::calloc(bytes / 4, 4);
  keep(p);
  check(p && filled(p, bytes, 0), "calloc", "250 x 4 B is zeroed");
  std::free(p);

  errno = 0;
  p = std::calloc(huge, 4);
  keep(p);
  // errno varies here (mimalloc leaves it 0), and isn't the shim's to set.
  check(p == nullptr, "calloc", "n * size overflowing: null (errno %d)", errno);
}

void checkRealloc() {
  void *p = std::realloc(nullptr, 64);
  keep(p);
  bool ok = p != nullptr;
  if (ok) {
    std::memset(p, 0x5a, 64);
    p = std::realloc(p, 1 << 16);
    keep(p);
    ok = p && filled(p, 64, 0x5a);
  }
  if (ok) {
    p = std::realloc(p, 16);
    keep(p);
    ok = p && filled(p, 16, 0x5a);
  }
  check(ok, "realloc",
        "from nullptr, grown to 64 KiB and shrunk to 16 B, contents kept");
  std::free(p);

  // realloc(p, 0) is implementation-defined (glibc frees and returns null,
  // jemalloc returns a minimal block); only its result is freed.
  void *q = std::malloc(32);
  keep(q);
  std::free(std::realloc(q, 0));

  void *r = std::malloc(32);
  keep(r);
  std::memset(r, 0x11, 32);
  errno = 0;
  void *grown = reallocarray(opaque(r), huge, 4);
  // errno varies here too (mimalloc sets EOVERFLOW).
  check(grown == nullptr && filled(r, 32, 0x11), "realloc",
        "reallocarray overflowing: null, old block untouched (errno %d)",
        errno);
  grown = reallocarray(opaque(r), 1000, 8);
  keep(grown);
  check(grown && filled(grown, 32, 0x11), "realloc",
        "reallocarray grown to 1000 x 8 B, contents kept");
  std::free(grown ? grown : r);
}

void checkAligned() {
  bool ok = true;
  for (std::size_t align : {16uz, 64uz, 256uz, pageBytes}) {
    void *a = memalign(align, 100);
    void *b = std::aligned_alloc(align, align * 2);
    void *c = nullptr;
    int rc = posix_memalign(&c, align, 100);
    keep(a);
    keep(b);
    keep(c);
    ok = ok && a && b && rc == 0 && c && aligned(a, align) &&
         aligned(b, align) && aligned(c, align);
    std::free(a);
    std::free(b);
    std::free(c);
  }
  check(ok, "aligned",
        "memalign, aligned_alloc, posix_memalign at 16 to 4096 B alignment");

  void *p = nullptr;
  int notPowerOfTwo = posix_memalign(&p, 24, 100);
  int belowPointer = posix_memalign(&p, sizeof(void *) / 2, 100);
  check(notPowerOfTwo == EINVAL && belowPointer == EINVAL, "aligned",
        "posix_memalign returns EINVAL for alignment 24 (%d) and %zu (%d)",
        notPowerOfTwo, sizeof(void *) / 2, belowPointer);
  int tooBig = posix_memalign(&p, 64, huge);
  check(tooBig == ENOMEM, "aligned",
        "posix_memalign returns ENOMEM for a huge request (%d)", tooBig);

  void *v = valloc(100);
  void *pv = pvalloc(100);
  keep(v);
  keep(pv);
  check(v && pv && aligned(v, pageBytes) && aligned(pv, pageBytes) &&
            malloc_usable_size(pv) >= pageBytes,
        "valloc", "valloc and pvalloc are page-aligned; pvalloc covers a page");
  std::free(v);
  std::free(pv);
}

// C23 frees: correctness is that nothing crashes or leaks into the wrong
// allocator; under mimalloc these run through the shim's fallbacks.
void checkSizedFree() {
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 43)
  for (int i = 0; i < formRounds; ++i) {
    void *p = std::malloc(64);
    keep(p);
    free_sized(p, 64);
    void *q = std::aligned_alloc(64, 128);
    keep(q);
    free_aligned_sized(q, 64, 128);
  }
  check(true, "freesized", "free_sized and free_aligned_sized, %d rounds",
        formRounds);
#else
  skip("freesized", "free_sized needs glibc 2.43");
#endif
}

void checkOperatorForms() {
  constexpr std::align_val_t align64{64};
  bool ok = true;
  for (int i = 0; i < formRounds; ++i) {
    std::array<void *, 12> p = {
        ::operator new(48),
        ::operator new(48),
        ::operator new[](48),
        ::operator new[](48),
        ::operator new(48, std::nothrow),
        ::operator new[](48, std::nothrow),
        ::operator new(48, align64),
        ::operator new(48, align64),
        ::operator new[](48, align64),
        ::operator new[](48, align64),
        ::operator new(48, align64, std::nothrow),
        ::operator new[](48, align64, std::nothrow),
    };
    for (std::size_t j = 0; j < p.size(); ++j) {
      keep(p[j]);
      ok = ok && p[j] && aligned(p[j], j >= 6 ? 64 : alignof(std::max_align_t));
    }
    ::operator delete(p[0]);
    ::operator delete(p[1], 48);
    ::operator delete[](p[2]);
    ::operator delete[](p[3], 48);
    ::operator delete(p[4], std::nothrow);
    ::operator delete[](p[5], std::nothrow);
    ::operator delete(p[6], align64);
    ::operator delete(p[7], 48, align64);
    ::operator delete[](p[8], align64);
    ::operator delete[](p[9], 48, align64);
    ::operator delete(p[10], align64, std::nothrow);
    ::operator delete[](p[11], align64, std::nothrow);
  }
  check(ok, "operators",
        "all 8 new forms paired with all 12 delete forms, %d rounds, aligned",
        formRounds);

  void *page = ::operator new(100, std::align_val_t{pageBytes});
  keep(page);
  check(aligned(page, pageBytes), "operators",
        "new with align_val_t{4096} is page-aligned");
  ::operator delete(page, std::align_val_t{pageBytes});
}

// Out-of-memory probes, run in a child. Each exits 0 when the call behaved as
// the standard describes; an allocator that aborts instead ends the child
// with a signal.
int runProbe(std::string_view probe) {
  if (probe == "new") {
    try {
      keep(::operator new(huge));
    } catch (const std::bad_alloc &) {
      return 0;
    }
    return 1;
  }
  if (probe == "nothrow")
    return ::operator new(huge, std::nothrow) == nullptr ? 0 : 1;
  if (probe == "aligned") {
    try {
      keep(::operator new(huge, std::align_val_t{64}));
    } catch (const std::bad_alloc &) {
      return 0;
    }
    return 1;
  }
  if (probe == "handler") {
    static int calls = 0;
    std::set_new_handler([] {
      ++calls;
      std::set_new_handler(nullptr);
    });
    try {
      keep(::operator new(huge));
    } catch (const std::bad_alloc &) {
      return calls == 1 ? 0 : 2;
    }
    return 1;
  }
  return 3;
}

// LD_PRELOAD with every entry naming the shim removed.
std::string preloadWithoutShim(std::string_view preload) {
  std::string out;
  for (const std::string &entry : preloadEntries(preload)) {
    if (entry.find(shimName) != std::string::npos)
      continue;
    if (!out.empty())
      out += ' ';
    out += entry;
  }
  return out;
}

// Runs this binary with --probe=<name>, with or without the shim, and returns
// its wait status. stderr is silenced (allocators print reports when they
// abort) and core dumps are off.
int spawnProbe(const char *probe, bool withShim) {
  std::vector<std::string> env;
  for (char **e = environ; *e; ++e) {
    std::string_view entry = *e;
    if (!entry.starts_with("LD_PRELOAD=")) {
      env.emplace_back(entry);
      continue;
    }
    std::string preload(entry.substr(11));
    if (!withShim)
      preload = preloadWithoutShim(preload);
    if (!preload.empty())
      env.push_back("LD_PRELOAD=" + preload);
  }
  std::vector<char *> envp;
  for (std::string &entry : env)
    envp.push_back(entry.data());
  envp.push_back(nullptr);
  std::string arg = std::string("--probe=") + probe;
  char self[] = "/proc/self/exe";
  char *argv[] = {self, arg.data(), nullptr};

  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    rlimit noCore{0, 0};
    ::setrlimit(RLIMIT_CORE, &noCore);
    if (int devNull = ::open("/dev/null", O_WRONLY); devNull >= 0)
      ::dup2(devNull, STDERR_FILENO);
    ::execve(self, argv, envp.data());
    ::_exit(127);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  return status;
}

std::string describe(int status) {
  if (status == -1)
    return "fork failed";
  if (WIFSIGNALED(status))
    return std::string("killed by ") + sigabbrev_np(WTERMSIG(status));
  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    return code == 0 ? "behaved as specified"
                     : "exited " + std::to_string(code);
  }
  return "ended oddly";
}

void checkOutOfMemory() {
  struct Probe {
    const char *name;
    const char *what;
  };
  for (Probe probe :
       {Probe{"new", "huge new"}, Probe{"aligned", "huge aligned new"},
        Probe{"nothrow", "huge nothrow new"},
        Probe{"handler", "new_handler, then bad_alloc"}}) {
    int shimmed = spawnProbe(probe.name, true);
    int plain = spawnProbe(probe.name, false);
    check(shimmed == plain && shimmed != -1, "oom",
          "%s: %s; without the shim: %s", probe.what, describe(shimmed).c_str(),
          describe(plain).c_str());
  }
}

// Threads allocate through a mix of entry points and free each other's
// blocks through a shared mailbox. Each block is filled with its owner's tag
// and checked before it's freed.
enum class Via { Malloc, Calloc, New, NewArray, AlignedNew, PosixMemalign };

struct Block {
  void *p;
  std::size_t bytes;
  Via via;
  unsigned char tag;
};

void release(const Block &block) {
  switch (block.via) {
  case Via::Malloc:
  case Via::Calloc:
  case Via::PosixMemalign:
    std::free(block.p);
    return;
  case Via::New:
    ::operator delete(block.p, block.bytes);
    return;
  case Via::NewArray:
    ::operator delete[](block.p);
    return;
  case Via::AlignedNew:
    ::operator delete(block.p, std::align_val_t{64});
    return;
  }
}

Block acquire(std::mt19937 &rng, unsigned char tag) {
  std::size_t bytes = std::uniform_int_distribution<std::size_t>(1, 512)(rng);
  auto via = static_cast<Via>(std::uniform_int_distribution<int>(0, 5)(rng));
  void *p = nullptr;
  switch (via) {
  case Via::Malloc:
    p = std::malloc(bytes);
    break;
  case Via::Calloc:
    p = std::calloc(1, bytes);
    break;
  case Via::New:
    p = ::operator new(bytes);
    break;
  case Via::NewArray:
    p = ::operator new[](bytes);
    break;
  case Via::AlignedNew:
    p = ::operator new(bytes, std::align_val_t{64});
    break;
  case Via::PosixMemalign:
    if (posix_memalign(&p, 64, bytes) != 0)
      p = nullptr;
    break;
  }
  if (p)
    std::memset(p, tag, bytes);
  return {p, bytes, via, tag};
}

void checkThreads() {
  std::mutex mailboxLock;
  std::vector<Block> mailbox;
  std::atomic<int> bad = 0;
  std::atomic<long> crossFrees = 0;

  auto worker = [&](int id) {
    std::mt19937 rng(static_cast<unsigned>(id) * 7919u);
    auto tag = static_cast<unsigned char>(id + 1);
    std::vector<Block> mine;
    auto drop = [&](const Block &block) {
      if (!block.p || !filled(block.p, block.bytes, block.tag))
        ++bad;
      if (block.p)
        release(block);
    };
    for (int op = 0; op < opsPerThread; ++op) {
      if (mine.size() < 256 && rng() % 3 != 0) {
        mine.push_back(acquire(rng, tag));
      } else if (!mine.empty()) {
        std::size_t i = rng() % mine.size();
        drop(mine[i]);
        mine[i] = mine.back();
        mine.pop_back();
      }
      if (op % 1024 == 1023 && mine.size() >= handoffBatch) {
        std::vector<Block> theirs;
        {
          std::lock_guard lock(mailboxLock);
          theirs.swap(mailbox);
          mailbox.assign(mine.end() - handoffBatch, mine.end());
        }
        mine.resize(mine.size() - handoffBatch);
        crossFrees += static_cast<long>(theirs.size());
        for (const Block &block : theirs)
          drop(block);
      }
    }
    for (const Block &block : mine)
      drop(block);
  };

  std::vector<std::thread> threads;
  for (int id = 0; id < threadCount; ++id)
    threads.emplace_back(worker, id);
  for (std::thread &thread : threads)
    thread.join();
  for (const Block &block : mailbox) {
    if (!filled(block.p, block.bytes, block.tag))
      ++bad;
    release(block);
  }
  check(bad == 0, "threads",
        "%d threads x %d ops, %ld blocks freed by another thread, %d corrupt "
        "or failed",
        threadCount, opsPerThread, crossFrees.load(), bad.load());
}

void printInfo() {
  auto bootstrapBytes = reinterpret_cast<std::size_t (*)()>(
      dlsym(RTLD_DEFAULT, "tatrace_bootstrap_bytes"));
  auto maxDepth = reinterpret_cast<unsigned (*)()>(
      dlsym(RTLD_DEFAULT, "tatrace_max_depth"));
  const char *preload = std::getenv("LD_PRELOAD");
  std::printf("INFO  preload   %s\n", preload ? preload : "(unset)");
  std::printf("INFO  shim      bootstrap %zu B, deepest nesting %u\n",
              bootstrapBytes ? bootstrapBytes() : 0, maxDepth ? maxDepth() : 0);
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 1 && std::string_view(argv[1]).starts_with("--probe="))
    return runProbe(std::string_view(argv[1]).substr(8));

  const void *base = shimBase();
  if (!base) {
    check(false, "shim",
          "libtieralloc-trace.so isn't loaded; run with it in LD_PRELOAD");
    return finish();
  }
  checkPreloaded();
  checkInterposed(base);
  checkMalloc();
  checkCalloc();
  checkRealloc();
  checkAligned();
  checkSizedFree();
  checkOperatorForms();
  checkOutOfMemory();
  checkThreads();
  printInfo();
  return finish();
}
