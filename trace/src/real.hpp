#pragma once

// defines a table of real alloc function pointers and how they get resolved, as
// well as their fallbacks if the next loaded library does not define it. Also
// defines the bootstrap buffer

#include "ta/contracts.hpp"

#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include <dlfcn.h>    // dlsym, RTLD_NEXT, etc
#include <malloc.h>   // memalign, pvalloc, malloc_usable_size, for the checks
#include <sys/auxv.h> // getauxval
#include <unistd.h>   // STDERR_FILENO for fatal messages

namespace tatrace::shim {

// C
using MallocFn = void *(std::size_t) noexcept;
using FreeFn = void(void *) noexcept;
using CallocFn = void *(std::size_t, std::size_t) noexcept;
using ReallocFn = void *(void *, std::size_t) noexcept;
using MemalignFn = void *(std::size_t, std::size_t) noexcept;
using PosixMemalignFn = int(void **, std::size_t, std::size_t) noexcept;
using AlignedAllocFn = void *(std::size_t, std::size_t) noexcept;
using MallocUsableSizeFn = std::size_t(void *) noexcept;
using ReallocarrayFn = void *(void *, std::size_t, std::size_t) noexcept;
using VallocFn = void *(std::size_t) noexcept;
using PvallocFn = void *(std::size_t) noexcept;
using FreeSizedFn = void(void *, std::size_t) noexcept;
using FreeAlignedSizedFn = void(void *, std::size_t, std::size_t) noexcept;

// C++; most can throw bad_alloc
using OpNewFn = void *(std::size_t);
using OpNewNothrowFn = void *(std::size_t, const std::nothrow_t &) noexcept;
using OpNewAlignedFn = void *(std::size_t, std::align_val_t);
using OpNewAlignedNothrowFn = void *(std::size_t, std::align_val_t,
                                     const std::nothrow_t &) noexcept;
using OpDeleteFn = void(void *) noexcept;
using OpDeleteNothrowFn = void(void *, const std::nothrow_t &) noexcept;
using OpDeleteSizedFn = void(void *, std::size_t) noexcept;
using OpDeleteAlignedFn = void(void *, std::align_val_t) noexcept;
using OpDeleteSizedAlignedFn = void(void *, std::size_t,
                                    std::align_val_t) noexcept;
using OpDeleteAlignedNothrowFn = void(void *, std::align_val_t,
                                      const std::nothrow_t &) noexcept;

// glibc declares these noexcept (__THROW), so the aliases must be too; a
// mismatched parameter or return type fails to convert here.
template <class Fn> consteval bool hasSignature(Fn *) { return true; }
static_assert(hasSignature<MallocFn>(&::malloc));
static_assert(hasSignature<FreeFn>(&::free));
static_assert(hasSignature<CallocFn>(&::calloc));
static_assert(hasSignature<ReallocFn>(&::realloc));
static_assert(hasSignature<MemalignFn>(&::memalign));
static_assert(hasSignature<PosixMemalignFn>(&::posix_memalign));
static_assert(hasSignature<AlignedAllocFn>(&::aligned_alloc));
static_assert(hasSignature<MallocUsableSizeFn>(&::malloc_usable_size));
static_assert(hasSignature<ReallocarrayFn>(&::reallocarray));
static_assert(hasSignature<VallocFn>(&::valloc));
static_assert(hasSignature<PvallocFn>(&::pvalloc));
#if __GLIBC_PREREQ(2, 43)
static_assert(hasSignature<FreeSizedFn>(&::free_sized));
static_assert(hasSignature<FreeAlignedSizedFn>(&::free_aligned_sized));
#endif
static_assert(hasSignature<OpNewFn>(&::operator new));
static_assert(hasSignature<OpNewFn>(&::operator new[]));
static_assert(hasSignature<OpNewNothrowFn>(&::operator new));
static_assert(hasSignature<OpNewNothrowFn>(&::operator new[]));
static_assert(hasSignature<OpNewAlignedFn>(&::operator new));
static_assert(hasSignature<OpNewAlignedFn>(&::operator new[]));
static_assert(hasSignature<OpNewAlignedNothrowFn>(&::operator new));
static_assert(hasSignature<OpNewAlignedNothrowFn>(&::operator new[]));
static_assert(hasSignature<OpDeleteFn>(&::operator delete));
static_assert(hasSignature<OpDeleteFn>(&::operator delete[]));
static_assert(hasSignature<OpDeleteSizedFn>(&::operator delete));
static_assert(hasSignature<OpDeleteSizedFn>(&::operator delete[]));
static_assert(hasSignature<OpDeleteAlignedFn>(&::operator delete));
static_assert(hasSignature<OpDeleteAlignedFn>(&::operator delete[]));
static_assert(hasSignature<OpDeleteNothrowFn>(&::operator delete));
static_assert(hasSignature<OpDeleteNothrowFn>(&::operator delete[]));
static_assert(hasSignature<OpDeleteSizedAlignedFn>(&::operator delete));
static_assert(hasSignature<OpDeleteSizedAlignedFn>(&::operator delete[]));
static_assert(hasSignature<OpDeleteAlignedNothrowFn>(&::operator delete));
static_assert(hasSignature<OpDeleteAlignedNothrowFn>(&::operator delete[]));

// dlsym returns void *; casting it to a function pointer assumes equal size.
static_assert(sizeof(MallocFn *) == sizeof(void *));

// each table load must be one plain load; a lock-based atomic (libatomic)
// would put a lock inside malloc.
template <class... Fns>
inline constexpr bool alwaysLockFree =
    (std::atomic<Fns *>::is_always_lock_free && ...);
static_assert(
    alwaysLockFree<MallocFn, FreeFn, CallocFn, ReallocFn, MemalignFn,
                   PosixMemalignFn, AlignedAllocFn, MallocUsableSizeFn,
                   ReallocarrayFn, VallocFn, PvallocFn, FreeSizedFn,
                   FreeAlignedSizedFn, OpNewFn, OpNewNothrowFn, OpNewAlignedFn,
                   OpNewAlignedNothrowFn, OpDeleteFn, OpDeleteSizedFn,
                   OpDeleteAlignedFn, OpDeleteNothrowFn, OpDeleteSizedAlignedFn,
                   OpDeleteAlignedNothrowFn>);

struct RealAllocator {
  std::atomic<MallocFn *> malloc;
  std::atomic<FreeFn *> free;
  std::atomic<CallocFn *> calloc;
  std::atomic<ReallocFn *> realloc;
  std::atomic<MemalignFn *> memalign;
  std::atomic<PosixMemalignFn *> posixMemalign;
  std::atomic<AlignedAllocFn *> alignedAlloc;
  std::atomic<MallocUsableSizeFn *> mallocUsableSize;
  std::atomic<ReallocarrayFn *> reallocarray;
  std::atomic<VallocFn *> valloc;
  std::atomic<PvallocFn *> pvalloc;
  std::atomic<FreeSizedFn *> freeSized;
  std::atomic<FreeAlignedSizedFn *> freeAlignedSized;
  // one member per symbol: new[] forwards to the real new[], not new
  std::atomic<OpNewFn *> opNew;
  std::atomic<OpNewFn *> opNewArray;
  std::atomic<OpNewNothrowFn *> opNewNothrow;
  std::atomic<OpNewNothrowFn *> opNewArrayNothrow;
  std::atomic<OpNewAlignedFn *> opNewAligned;
  std::atomic<OpNewAlignedFn *> opNewArrayAligned;
  std::atomic<OpNewAlignedNothrowFn *> opNewAlignedNothrow;
  std::atomic<OpNewAlignedNothrowFn *> opNewArrayAlignedNothrow;
  std::atomic<OpDeleteFn *> opDelete;
  std::atomic<OpDeleteFn *> opDeleteArray;
  std::atomic<OpDeleteNothrowFn *> opDeleteNothrow;
  std::atomic<OpDeleteNothrowFn *> opDeleteArrayNothrow;
  std::atomic<OpDeleteSizedFn *> opDeleteSized;
  std::atomic<OpDeleteSizedFn *> opDeleteArraySized;
  std::atomic<OpDeleteAlignedFn *> opDeleteAligned;
  std::atomic<OpDeleteAlignedFn *> opDeleteArrayAligned;
  std::atomic<OpDeleteSizedAlignedFn *> opDeleteSizedAligned;
  std::atomic<OpDeleteSizedAlignedFn *> opDeleteArraySizedAligned;
  std::atomic<OpDeleteAlignedNothrowFn *> opDeleteAlignedNothrow;
  std::atomic<OpDeleteAlignedNothrowFn *> opDeleteArrayAlignedNothrow;

  // atomic: racing resolvers both store it. relaxed is enough, since the
  // release stores of the pointers publish it.
  std::atomic<std::size_t> pageSize;
};

inline constinit RealAllocator real{};
// to prevent recursive internal malloc calls during resolution.
[[gnu::tls_model(
    "initial-exec")]] inline constinit thread_local bool resolving = false;

// bootstrap constants
inline constexpr std::size_t bootstrapBytes = 64 * 1024;
inline constexpr std::size_t bootstrapAlign = 16;
inline constexpr std::size_t bootstrapHeader = 16;
alignas(bootstrapAlign) inline constinit std::byte
    bootstrapBuffer[bootstrapBytes];
inline constinit std::atomic<std::size_t> bootstrapUsed = 0;

static_assert(bootstrapAlign >= alignof(std::max_align_t));
static_assert(std::has_single_bit(bootstrapAlign));
static_assert(bootstrapHeader % bootstrapAlign == 0 &&
              bootstrapHeader >= sizeof(std::size_t));
static_assert(std::atomic<std::size_t>::is_always_lock_free);

// checks if p is contained within the buffer
inline bool isBootstrap(const void *p) noexcept {
  auto offset = reinterpret_cast<std::uintptr_t>(p) -
                reinterpret_cast<std::uintptr_t>(bootstrapBuffer);
  return offset < bootstrapBytes;
}
void resolveAll() noexcept;
void *bootstrapAlloc(std::size_t bytes, std::size_t align) noexcept
    TA_PRE(std::has_single_bit(align));
std::size_t bootstrapSize(const void *p) noexcept TA_PRE(isBootstrap(p));

} // namespace tatrace::shim
