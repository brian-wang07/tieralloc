#pragma once

// Macros for C++26 contract assertions (P2900), shared by the bench and the
// profiler, so project code can state its preconditions while still parsing on
// compilers without contracts: clangd and clang-tidy, and Clang sanitizer
// builds.
//
// - TA_PRE(cond) goes on a declaration, after noexcept and before the body:
//     void *allocate(std::size_t bytes, std::size_t align)
//       TA_PRE(std::has_single_bit(align));
//   Without contracts it expands to nothing. A precondition has no body to
//   fall back into, and the measured and tested compiler (GCC 16) has them.
// - TA_POST(r, cond) also goes on a declaration; r names the return value:
//     void *allocate(std::size_t bytes, const std::size_t align)
//       TA_POST(r, reinterpret_cast<std::uintptr_t>(r) % align == 0);
//   A value parameter used in cond must be declared const, in every
//   declaration, or GCC rejects it. Without contracts it expands to nothing.
// - TA_ASSERT(cond) is a statement inside a function body. Without contracts it
//   falls back to a manual check in builds without NDEBUG.
//
// No contracts in tieralloc.hpp: it must stay compatible with C++20.

#include <cstdio>
#include <cstdlib>

// GCC 15 defines __cpp_contracts as 201906, for the old pre-standard syntax,
// which these macros do not use.
#if defined(__cpp_contracts) && __cpp_contracts >= 202502L
#define TA_HAS_CONTRACTS 1
#define TA_PRE(...) pre(__VA_ARGS__)
#define TA_POST(result, ...) post(result : __VA_ARGS__)
#define TA_ASSERT(...) contract_assert(__VA_ARGS__)
#else
#define TA_HAS_CONTRACTS 0
#define TA_PRE(...)
#define TA_POST(result, ...)
#if defined(NDEBUG)
// Unevaluated, so variables used only in assertions do not warn as unused.
#define TA_ASSERT(...) static_cast<void>(sizeof((__VA_ARGS__) ? 1 : 0))
#else
#define TA_ASSERT(...)                                                         \
  ((__VA_ARGS__)                                                               \
       ? static_cast<void>(0)                                                  \
       : ::ta::detail::assertionFailed(#__VA_ARGS__, __FILE__, __LINE__))
#endif
#endif

// The evaluation semantic, for the record's "build" field.
#ifndef TA_CONTRACT_SEMANTIC
#if TA_HAS_CONTRACTS
#define TA_CONTRACT_SEMANTIC "enforce" // GCC's default without the flag.
#else
#define TA_CONTRACT_SEMANTIC "none"
#endif
#endif

namespace ta::detail {

// Out of line so the failure path does not bloat every inlined check.
[[noreturn, gnu::cold, gnu::noinline]] inline void
assertionFailed(const char *expression, const char *file, int line) noexcept {
  std::fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expression);
  std::abort();
}

} // namespace ta::detail
