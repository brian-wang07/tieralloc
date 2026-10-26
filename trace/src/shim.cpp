// implements the shim that intercepts any allocation/free call.

#include "real.hpp"
#include "tatrace/pool.hpp"

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

// exported, and keeps its own stack frame
#define TA_HOOK [[gnu::visibility("default"), gnu::noinline]]

#define TA_EXPORT [[gnu::visibility("default")]]

namespace tatrace::shim {
namespace {

void writeMessage(const char *msg, const char *detail) noexcept {
  write(STDERR_FILENO, "tatrace: ", 9);
  write(STDERR_FILENO, msg, std::strlen(msg));
  if (detail) {
    write(STDERR_FILENO, ": ", 2);
    write(STDERR_FILENO, detail, std::strlen(detail));
  }
  write(STDERR_FILENO, "\n", 1);
}

[[noreturn, gnu::cold]] void fatal(const char *msg,
                                   const char *detail = nullptr) noexcept {
  writeMessage(msg, detail);
  std::abort();
}

[[gnu::cold]] void warn(const char *msg,
                        const char *detail = nullptr) noexcept {
  writeMessage(msg, detail);
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
    return nullptr;
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

// this process's mapping of the watcher's pool; header stays null when
// tracing is off (no launcher, a grandchild, or a failed attach).
constinit Pool pool{};

// before main, so the program must not see errno change. MapFailed is the
// one failure worth a line: only the real target gets that far (the parent
// check comes first), and errno still holds mmap's reason, since attachPool
// returns straight after the failed mmap with nothing to clean up.
void attachFromEnv() noexcept {
  int programErrno = errno;
  auto attached = attachPool(parsePoolFd(std::getenv(poolFdEnv)));
  int attachErrno = errno;
  if (attached)
    pool = *attached;
  else if (attached.error() == AttachError::MapFailed)
    // strerrordesc_np, not strerror: no locale lookup, so no allocation
    warn("cannot map the trace pool, tracing off",
         strerrordesc_np(attachErrno));
  errno = programErrno;
}

[[gnu::constructor(101)]] void init() noexcept {
  resolveAll();
  attachFromEnv();

  // safety check: if another malloc wins symbol lookup, raise warning
  Dl_info winner;
  void *m = dlsym(RTLD_DEFAULT, "malloc");
  if (m == nullptr || dladdr(m, &winner) == 0)
    // cant tell
    return;
  // our own base
  const void *self = objectBase(reinterpret_cast<const void *>(&init));
  if (winner.dli_fbase != self)
    warn("malloc is served by another library. list the trace first in "
         "LD_PRELOAD",
         winner.dli_fname);
}

void *allocate(std::size_t n) noexcept {
  if (resolving) [[unlikely]]
    return bootstrapAlloc(n, bootstrapAlign);
  return resolved<&RealAllocator::malloc>()(n);
}

void *reallocBootstrap(void *p, std::size_t n) noexcept {
  if (n == 0)
    return nullptr;
  void *q = allocate(n);
  if (q)
    std::memcpy(q, p, std::min(bootstrapSize(p), n));
  return q;
}

template <class Fn>
Fn *requireFrom(const char *name, const void *base) noexcept {
  Fn *f = lookup<Fn>(name);
  if (!f)
    fatal("no next definition", name);
  if (objectBase(reinterpret_cast<const void *>(f)) != base)
    fatal("not from the same library as malloc", name);
  return f;
}

template <class Fn>
Fn *preferFrom(const char *name, const void *base, Fn *fallback) noexcept {
  Fn *f = lookup<Fn>(name);
  if (f && objectBase(reinterpret_cast<const void *>(f)) == base)
    return f;
  return fallback;
}

template <class Fn> Fn *nextOr(const char *name, Fn *fallback) noexcept {
  Fn *f = lookup<Fn>(name);
  return f ? f : fallback;
}

// helper for hooks. decltype(auto) so posix_memalign's int comes back as is.
template <auto Member, class... Args>
[[gnu::always_inline]] inline decltype(auto)
forwardAlloc(Args... args) noexcept(noexcept(resolved<Member>()(args...))) {
  Reentry g;
  // dont need to forward args as args is always trivially copyable
  return resolved<Member>()(args...);
}

// ...and of every freeing hook: bootstrap blocks are never forwarded
template <auto Member, class... Args>
[[gnu::always_inline]] inline void forwardFree(void *p, Args... args) noexcept {
  if (isBootstrap(p)) [[unlikely]]
    return;
  Reentry g;
  resolved<Member>()(p, args...);
}

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
    fatal("no next definition", "malloc");

  const void *base = objectBase(reinterpret_cast<const void *>(m));
  // required, as it is part of the same library as malloc
  auto f = requireFrom<FreeFn>("free", base);
  auto c = requireFrom<CallocFn>("calloc", base);
  auto r = requireFrom<ReallocFn>("realloc", base);
  auto ma = requireFrom<MemalignFn>("memalign", base);
  auto pma = requireFrom<PosixMemalignFn>("posix_memalign", base);
  auto aa = requireFrom<AlignedAllocFn>("aligned_alloc", base);
  auto mus = requireFrom<MallocUsableSizeFn>("malloc_usable_size", base);

  // optional: uses the allocator's if defined, else fallback
  auto ra =
      preferFrom<ReallocarrayFn>("reallocarray", base, fallbackReallocarray);
  auto va = preferFrom<VallocFn>("valloc", base, fallbackValloc);
  auto pva = preferFrom<PvallocFn>("pvalloc", base, fallbackPvalloc);
  auto fs = preferFrom<FreeSizedFn>("free_sized", base, fallbackFreeSized);
  auto fas = preferFrom<FreeAlignedSizedFn>("free_aligned_sized", base,
                                            fallbackFreeAlignedSized);

  // C++

  auto n = nextOr<OpNewFn>("_Znwm", fallbackNew);
  auto nr = nextOr<OpNewFn>("_Znam", fallbackNew);
  auto nn = nextOr<OpNewNothrowFn>("_ZnwmRKSt9nothrow_t", fallbackNewNothrow);
  auto nrt = nextOr<OpNewNothrowFn>("_ZnamRKSt9nothrow_t", fallbackNewNothrow);
  auto na = nextOr<OpNewAlignedFn>("_ZnwmSt11align_val_t", fallbackNewAligned);
  auto nra = nextOr<OpNewAlignedFn>("_ZnamSt11align_val_t", fallbackNewAligned);
  auto nan = nextOr<OpNewAlignedNothrowFn>("_ZnwmSt11align_val_tRKSt9nothrow_t",
                                           fallbackNewAlignedNothrow);
  auto nran = nextOr<OpNewAlignedNothrowFn>(
      "_ZnamSt11align_val_tRKSt9nothrow_t", fallbackNewAlignedNothrow);

  auto d = nextOr<OpDeleteFn>("_ZdlPv", fallbackDelete);
  auto dr = nextOr<OpDeleteFn>("_ZdaPv", fallbackDelete);
  auto dn =
      nextOr<OpDeleteNothrowFn>("_ZdlPvRKSt9nothrow_t", fallbackDeleteNothrow);
  auto drn =
      nextOr<OpDeleteNothrowFn>("_ZdaPvRKSt9nothrow_t", fallbackDeleteNothrow);
  auto ds = nextOr<OpDeleteSizedFn>("_ZdlPvm", fallbackDeleteSized);
  auto drs = nextOr<OpDeleteSizedFn>("_ZdaPvm", fallbackDeleteSized);
  auto da =
      nextOr<OpDeleteAlignedFn>("_ZdlPvSt11align_val_t", fallbackDeleteAligned);
  auto dra =
      nextOr<OpDeleteAlignedFn>("_ZdaPvSt11align_val_t", fallbackDeleteAligned);
  auto dsa = nextOr<OpDeleteSizedAlignedFn>("_ZdlPvmSt11align_val_t",
                                            fallbackDeleteSizedAligned);
  auto drsa = nextOr<OpDeleteSizedAlignedFn>("_ZdaPvmSt11align_val_t",
                                             fallbackDeleteSizedAligned);
  auto dan = nextOr<OpDeleteAlignedNothrowFn>(
      "_ZdlPvSt11align_val_tRKSt9nothrow_t", fallbackDeleteAlignedNothrow);
  auto dran = nextOr<OpDeleteAlignedNothrowFn>(
      "_ZdaPvSt11align_val_tRKSt9nothrow_t", fallbackDeleteAlignedNothrow);

  // publish this first
  real.pageSize.store(getauxval(AT_PAGESZ), std::memory_order_relaxed);

  // required first as fallbacks call through them (need to be well defined
  // first)
  real.malloc.store(m, std::memory_order_release);
  real.free.store(f, std::memory_order_release);
  real.calloc.store(c, std::memory_order_release);
  real.realloc.store(r, std::memory_order_release);
  real.memalign.store(ma, std::memory_order_release);
  real.posixMemalign.store(pma, std::memory_order_release);
  real.alignedAlloc.store(aa, std::memory_order_release);
  real.mallocUsableSize.store(mus, std::memory_order_release);

  real.reallocarray.store(ra, std::memory_order_release);
  real.valloc.store(va, std::memory_order_release);
  real.pvalloc.store(pva, std::memory_order_release);
  real.freeSized.store(fs, std::memory_order_release);
  real.freeAlignedSized.store(fas, std::memory_order_release);

  // after the c functions: the c++ fallbacks call through malloc, free and
  // posix_memalign
  real.opNew.store(n, std::memory_order_release);
  real.opNewArray.store(nr, std::memory_order_release);
  real.opNewNothrow.store(nn, std::memory_order_release);
  real.opNewArrayNothrow.store(nrt, std::memory_order_release);
  real.opNewAligned.store(na, std::memory_order_release);
  real.opNewArrayAligned.store(nra, std::memory_order_release);
  real.opNewAlignedNothrow.store(nan, std::memory_order_release);
  real.opNewArrayAlignedNothrow.store(nran, std::memory_order_release);
  real.opDelete.store(d, std::memory_order_release);
  real.opDeleteArray.store(dr, std::memory_order_release);
  real.opDeleteNothrow.store(dn, std::memory_order_release);
  real.opDeleteArrayNothrow.store(drn, std::memory_order_release);
  real.opDeleteSized.store(ds, std::memory_order_release);
  real.opDeleteArraySized.store(drs, std::memory_order_release);
  real.opDeleteAligned.store(da, std::memory_order_release);
  real.opDeleteArrayAligned.store(dra, std::memory_order_release);
  real.opDeleteSizedAligned.store(dsa, std::memory_order_release);
  real.opDeleteArraySizedAligned.store(drsa, std::memory_order_release);
  real.opDeleteAlignedNothrow.store(dan, std::memory_order_release);
  real.opDeleteArrayAlignedNothrow.store(dran, std::memory_order_release);

  resolving = false;
}

} // namespace tatrace::shim

using namespace tatrace::shim;

// C hooks
extern "C" {
TA_HOOK void *malloc(std::size_t n) noexcept {
  if (resolving) [[unlikely]]
    return bootstrapAlloc(n, bootstrapAlign);
  return forwardAlloc<&RealAllocator::malloc>(n);
}

TA_HOOK void *calloc(std::size_t n, std::size_t m) noexcept {
  if (resolving) [[unlikely]] {
    std::size_t bytes;
    if (__builtin_mul_overflow(n, m, &bytes)) {
      errno = ENOMEM;
      return nullptr;
    }
    return bootstrapAlloc(bytes, bootstrapAlign);
  }
  return forwardAlloc<&RealAllocator::calloc>(n, m);
}

TA_HOOK void *realloc(void *p, std::size_t n) noexcept {
  if (isBootstrap(p)) [[unlikely]]
    return reallocBootstrap(p, n);
  if (resolving && p == nullptr) [[unlikely]]
    return bootstrapAlloc(n, bootstrapAlign);
  return forwardAlloc<&RealAllocator::realloc>(p, n);
}

TA_HOOK void *memalign(std::size_t align, std::size_t n) noexcept {
  if (resolving) [[unlikely]] {
    if (!std::has_single_bit(align)) {
      errno = EINVAL;
      return nullptr;
    }
    return bootstrapAlloc(n, align);
  }
  return forwardAlloc<&RealAllocator::memalign>(align, n);
}

TA_HOOK void *aligned_alloc(std::size_t align, std::size_t n) noexcept {
  if (resolving) [[unlikely]] {
    if (!std::has_single_bit(align)) {
      errno = EINVAL;
      return nullptr;
    }
    return bootstrapAlloc(n, align);
  }
  return forwardAlloc<&RealAllocator::alignedAlloc>(align, n);
}

TA_HOOK int posix_memalign(void **out, std::size_t align,
                           std::size_t n) noexcept {
  if (resolving) [[unlikely]] {
    if (!std::has_single_bit(align) || align % sizeof(void *) != 0)
      return EINVAL;
    *out = bootstrapAlloc(n, align);
    return 0;
  }
  return forwardAlloc<&RealAllocator::posixMemalign>(out, align, n);
}

TA_HOOK void *reallocarray(void *p, std::size_t n, std::size_t m) noexcept {
  if (isBootstrap(p)) [[unlikely]] {
    std::size_t bytes;
    if (__builtin_mul_overflow(n, m, &bytes)) {
      errno = ENOMEM;
      return nullptr;
    }
    return reallocBootstrap(p, bytes);
  }
  return forwardAlloc<&RealAllocator::reallocarray>(p, n, m);
}

TA_HOOK std::size_t malloc_usable_size(void *p) noexcept {
  // no reentry as this just queries
  if (p == nullptr)
    return 0;
  if (isBootstrap(p)) [[unlikely]]
    return bootstrapSize(p);
  return resolved<&RealAllocator::mallocUsableSize>()(p);
}

TA_HOOK void free_sized(void *p, std::size_t n) noexcept {
  forwardFree<&RealAllocator::freeSized>(p, n);
}

TA_HOOK void free_aligned_sized(void *p, std::size_t align,
                                std::size_t n) noexcept {
  forwardFree<&RealAllocator::freeAlignedSized>(p, align, n);
}

// never called by dlsym, so can be forwarded directly
TA_HOOK void *valloc(std::size_t n) noexcept {
  return forwardAlloc<&RealAllocator::valloc>(n);
}

TA_HOOK void *pvalloc(std::size_t n) noexcept {
  return forwardAlloc<&RealAllocator::pvalloc>(n);
}

TA_HOOK void free(void *p) noexcept { forwardFree<&RealAllocator::free>(p); }

TA_EXPORT std::size_t tatrace_bootstrap_bytes() noexcept {
  return bootstrapUsed.load(std::memory_order_relaxed);
}

TA_EXPORT unsigned tatrace_max_depth() noexcept {
  return maxDepth.load(std::memory_order_relaxed);
}
}

// C++ hooks

// throw new
TA_HOOK void *operator new(std::size_t n) {
  return forwardAlloc<&RealAllocator::opNew>(n);
}

TA_HOOK void *operator new[](std::size_t n) {
  return forwardAlloc<&RealAllocator::opNewArray>(n);
}

TA_HOOK void *operator new(std::size_t n, std::align_val_t a) {
  return forwardAlloc<&RealAllocator::opNewAligned>(n, a);
}

TA_HOOK void *operator new[](std::size_t n, std::align_val_t a) {
  return forwardAlloc<&RealAllocator::opNewArrayAligned>(n, a);
}

// nothrow new
TA_HOOK void *operator new(std::size_t n, const std::nothrow_t &t) noexcept {
  return forwardAlloc<&RealAllocator::opNewNothrow>(n, t);
}

TA_HOOK void *operator new[](std::size_t n, const std::nothrow_t &t) noexcept {
  return forwardAlloc<&RealAllocator::opNewArrayNothrow>(n, t);
}

TA_HOOK void *operator new(std::size_t n, std::align_val_t a,
                           const std::nothrow_t &t) noexcept {
  return forwardAlloc<&RealAllocator::opNewAlignedNothrow>(n, a, t);
}

TA_HOOK void *operator new[](std::size_t n, std::align_val_t a,
                             const std::nothrow_t &t) noexcept {
  return forwardAlloc<&RealAllocator::opNewArrayAlignedNothrow>(n, a, t);
}

// deletes
TA_HOOK void operator delete(void *p) noexcept {
  forwardFree<&RealAllocator::opDelete>(p);
}

TA_HOOK void operator delete[](void *p) noexcept {
  forwardFree<&RealAllocator::opDeleteArray>(p);
}

TA_HOOK void operator delete(void *p, const std::nothrow_t &t) noexcept {
  forwardFree<&RealAllocator::opDeleteNothrow>(p, t);
}

TA_HOOK void operator delete[](void *p, const std::nothrow_t &t) noexcept {
  forwardFree<&RealAllocator::opDeleteArrayNothrow>(p, t);
}

TA_HOOK void operator delete(void *p, std::size_t n) noexcept {
  forwardFree<&RealAllocator::opDeleteSized>(p, n);
}

TA_HOOK void operator delete[](void *p, std::size_t n) noexcept {
  forwardFree<&RealAllocator::opDeleteArraySized>(p, n);
}

TA_HOOK void operator delete(void *p, std::align_val_t a) noexcept {
  forwardFree<&RealAllocator::opDeleteAligned>(p, a);
}

TA_HOOK void operator delete[](void *p, std::align_val_t a) noexcept {
  forwardFree<&RealAllocator::opDeleteArrayAligned>(p, a);
}

TA_HOOK void operator delete(void *p, std::size_t n,
                             std::align_val_t a) noexcept {
  forwardFree<&RealAllocator::opDeleteSizedAligned>(p, n, a);
}

TA_HOOK void operator delete[](void *p, std::size_t n,
                               std::align_val_t a) noexcept {
  forwardFree<&RealAllocator::opDeleteArraySizedAligned>(p, n, a);
}

TA_HOOK void operator delete(void *p, std::align_val_t a,
                             const std::nothrow_t &t) noexcept {
  forwardFree<&RealAllocator::opDeleteAlignedNothrow>(p, a, t);
}

TA_HOOK void operator delete[](void *p, std::align_val_t a,
                               const std::nothrow_t &t) noexcept {
  forwardFree<&RealAllocator::opDeleteArrayAlignedNothrow>(p, a, t);
}
