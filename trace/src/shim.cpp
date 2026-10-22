// implements the shim that intercepts any allocation/free call.

#include "real.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include <dlfcn.h>
#include <sys/auxv.h>
#include <unistd.h>

#define TA_EXPORT [[gnu::visibility("default")]]

namespace tatrace::shim {
namespace {

[[noreturn, gnu::cold]] void fatal(const char *msg) noexcept {
  write(STDERR_FILENO, "tatrace: ", 9);
  write(STDERR_FILENO, msg, std::strlen(msg));
  write(STDERR_FILENO, "\n", 1);
  std::abort();
}

template <class Fn> Fn *lookup(const char *name) noexcept {
  return reinterpret_cast<Fn *>(dlsym(RTLD_NEXT, name));
}

const void *objectBase(const void *symbol) noexcept {
  // returns the base address in which symbol is loaded
  Dl_info info;
  return dladdr(symbol, &info) ? info.dli_fbase : nullptr;
}

template <auto Member>
[[gnu::cold, gnu::noinline]] auto resolveSlow() noexcept {
  if (resolving) [[unlikely]]
    fatal("re-entered resolution");
  resolveAll();
  auto f = (real.*Member).load(std::memory_order_acquire);
  if (!f)
    fatal("unresolved symbol");
  return f;
}

template <auto Member> [[gnu::always_inline]] inline auto resolved() noexcept {
  auto f = (real.*Member).load(std::memory_order_acquire);
  if (f) [[likely]]
    return f;
  return resolveSlow<Member>();
}

// reentrance guard: protects against internal mallocs getting recursively
// called infinitely
[[gnu::tls_model("initial-exec")]] constinit thread_local unsigned depth = 0;
constinit std::atomic<unsigned> maxDepth = 0;

struct Reentry {
  bool outer;
  Reentry() noexcept : outer(depth++ == 0) {
    if (!outer) {
      unsigned expected = maxDepth.load(std::memory_order_relaxed);
      while (expected < depth &&
             !maxDepth.compare_exchange_weak(expected, depth,
                                             std::memory_order_relaxed))
        ;
    }
  }

  ~Reentry() { --depth; }
  Reentry(const Reentry &) = delete;
  Reentry &operator=(const Reentry &) = delete;
};

// fallbacks, for when the next library doesn't define a symbol (or, for the
// optional c functions, defines it in a different library than malloc)
void *fallbackReallocarray(void *p, std::size_t n, std::size_t m) noexcept {
  std::size_t bytes;
  if (__builtin_mul_overflow(n, m, &bytes)) {
    errno = ENOMEM;
    the benchmark just allocates a bunch of return nullptr;
  }
  return resolved<&RealAllocator::realloc>()(p, bytes);
}

void *fallbackValloc(std::size_t n) noexcept {
  std::size_t page = real.pageSize.load(std::memory_order_relaxed);
  return resolved<&RealAllocator::memalign>()(page, n);
}

void *fallbackPvalloc(std::size_t n) noexcept {
  std::size_t page = real.pageSize.load(std::memory_order_relaxed);
  std::size_t bytes = std::max<std::size_t>(n, 1);
  if (bytes > SIZE_MAX - (page - 1)) {
    errno = ENOMEM;
    return nullptr;
  }
  bytes = (bytes + page - 1) & ~(page - 1);
  return resolved<&RealAllocator::memalign>()(page, bytes);
}

void fallbackFreeSized(void *p, std::size_t) noexcept {
  resolved<&RealAllocator::free>()(p);
}

void fallbackFreeAlignedSized(void *p, std::size_t, std::size_t) noexcept {
  resolved<&RealAllocator::free>()(p);
}

// operator new's contract: retry while a new_handler is installed, then throw.
// the handler may free memory, replace itself, or throw.
void *fallbackNew(std::size_t n) {
  for (;;) {
    if (void *p =
            resolved<&RealAllocator::malloc>()(std::max<std::size_t>(n, 1)))
      return p;
    std::new_handler handler = std::get_new_handler();
    if (!handler)
      throw std::bad_alloc();
    handler();
  }
}

void *fallbackNewNothrow(std::size_t n, const std::nothrow_t &) noexcept {
  try {
    return fallbackNew(n);
  } catch (...) {
    return nullptr;
  }
}

// posix_memalign, not aligned_alloc, which needs n to be a multiple of the
// alignment. posix_memalign needs at least sizeof(void *).
void *fallbackNewAligned(std::size_t n, std::align_val_t align) {
  std::size_t alignment =
      std::max(static_cast<std::size_t>(align), sizeof(void *));
  for (;;) {
    void *p = nullptr;
    if (resolved<&RealAllocator::posixMemalign>()(
            &p, alignment, std::max<std::size_t>(n, 1)) == 0)
      return p;
    std::new_handler handler = std::get_new_handler();
    if (!handler)
      throw std::bad_alloc();
    handler();
  }
}

void *fallbackNewAlignedNothrow(std::size_t n, std::align_val_t align,
                                const std::nothrow_t &) noexcept {
  try {
    return fallbackNewAligned(n, align);
  } catch (...) {
    return nullptr;
  }
}

// every delete ends in free; size and alignment are only hints.
void fallbackDelete(void *p) noexcept { resolved<&RealAllocator::free>()(p); }

void fallbackDeleteNothrow(void *p, const std::nothrow_t &) noexcept {
  resolved<&RealAllocator::free>()(p);
}

void fallbackDeleteSized(void *p, std::size_t) noexcept {
  resolved<&RealAllocator::free>()(p);
}

void fallbackDeleteAligned(void *p, std::align_val_t) noexcept {
  resolved<&RealAllocator::free>()(p);
}

void fallbackDeleteSizedAligned(void *p, std::size_t,
                                std::align_val_t) noexcept {
  resolved<&RealAllocator::free>()(p);
}

void fallbackDeleteAlignedNothrow(void *p, std::align_val_t,
                                  const std::nothrow_t &) noexcept {
  resolved<&RealAllocator::free>()(p);
}

[[gnu::constructor(101)]] void init() noexcept { resolveAll(); }

} // namespace

void *bootstrapAlloc(std::size_t bytes, std::size_t align) noexcept {
  align = std::max(align, bootstrapAlign);
  bytes = std::max<std::size_t>(bytes, 1);
  std::size_t used = bootstrapUsed.load(std::memory_order_relaxed);
  std::size_t start, end;
  for (;;) {
    // round up to alignment
    start = (used + bootstrapHeader + align - 1) & ~(align - 1);
    end = start + bytes;
    if (end > bootstrapBytes)
      fatal("bootstrap buffer exhausted");
    if (bootstrapUsed.compare_exchange_weak(used, end,
                                            std::memory_order_relaxed))
      break;
  }
  std::memcpy(bootstrapBuffer + start - sizeof(std::size_t), &bytes,
              sizeof(bytes));
  return bootstrapBuffer + start;
}

std::size_t bootstrapSize(const void *p) noexcept {
  std::size_t size;
  std::memcpy(&size, static_cast<const std::byte *>(p) - sizeof(size),
              sizeof(size));
  return size;
}

void resolveAll() noexcept {
  resolving = true;

  auto m = lookup<MallocFn>("malloc");
  if (!m) [[unlikely]]
    fatal("no next malloc");
  auto f = lookup<FreeFn>("free");
  if (!f) [[unlikely]]
    fatal("no next free");

  real.pageSize.store(getauxval(AT_PAGESZ), std::memory_order_relaxed);
  real.malloc.store(m, std::memory_order_release);
  real.free.store(f, std::memory_order_release);

  resolving = false;
}

} // namespace tatrace::shim

using namespace tatrace::shim;

// C hooks
extern "C" {
TA_EXPORT [[gnu::noinline]] void *malloc(std::size_t n) noexcept {
  if (resolving) [[unlikely]]
    return bootstrapAlloc(n, bootstrapAlign);
  return resolved<&RealAllocator::malloc>()(n);
}

TA_EXPORT [[gnu::noinline]] void free(void *p) noexcept {
  if (isBootstrap(p)) [[unlikely]]
    return;
  resolved<&RealAllocator::free>()(p);
}

TA_EXPORT std::size_t tatrace_bootstrap_bytes() noexcept {
  return bootstrapUsed.load(std::memory_order_relaxed);
}
}
