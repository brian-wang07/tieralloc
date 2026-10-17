# tieralloc

## Working with me

- **Don't write the implementation code unless I explicitly ask for it.** By default, give an outline of what needs to be done: components, responsibilities, interfaces, ordering, pitfalls. Small illustrative snippets are fine only when prose would be unclear.
- Build/config scaffolding (e.g. CMake) is fine to write when I ask for it.

## Code style

LLVM style, with one deviation: variables use lowerCamelCase.

- **Formatting:** clang-format with `.clang-format` (`BasedOnStyle: LLVM`): 2-space indent, 80 columns, `T *p` / `T &r` pointer and reference alignment.
- **Naming:**
  - Types, classes, structs, enums, concepts: `UpperCamelCase` (`ClockInfo`, `RegionResult`, `RegisterSized`).
  - Functions and methods: `lowerCamelCase` (`readTicks`, `doNotOptimize`).
  - Variables, data members and parameters: `lowerCamelCase` (`ticksPerNs`, `startTicks`, `value`). No prefixes or trailing underscores for members. **This differs from LLVM, which uses UpperCamelCase.**
  - Enumerators: `UpperCamelCase` (`State::Idle`); use `enum class`.
  - Namespaces: lowercase (`tabench`).
  - Macros: `UPPER_SNAKE_CASE` with a project prefix (`TA_`, `TIERALLOC_`).
  - Acronyms are upper-case inside a name and lower-case at the start of a lowerCamelCase name (`currentCPU`, `cpuBegin`).
  - Never use reserved identifiers: a leading underscore followed by a capital letter, or a double underscore anywhere.
- **Language standard:** C++23 for the bench, profiler and tooling. `tieralloc.hpp` must stay C++20-compatible (its tests build at C++20).
- **Headers:** `#pragma once`. Includes go project headers first, then standard library headers, then platform headers guarded by `#if`.
- **Comments:** `//`, written as sentences; explain *why*, not what.

## Project

A hotness- and lifetime-aware C++ allocator, plus a memory profiler and a benchmark suite to evaluate it. Solo project; the full design doc lives outside the repo (`~/Downloads/tieralloc.md`).

## Hypothesis

Compile-time hints about which objects are **hot** and which **die together** buy measurable wins:
- Packing hot objects densely shrinks the hot set's page/cache-line footprint and cuts dTLB misses.
- Segregating by lifetime makes pages empty together, lowering RSS and fragmentation.

Raw malloc/free cost is expected to be near mimalloc's, not better. The headline comparison is **tieralloc with hints vs. tieralloc without hints** on the same workload; external allocators are context.

## Deliverables

1. **`tieralloc.hpp`**: stb-style single-header library (`#define TIERALLOC_IMPLEMENTATION` in one TU). C++20 minimum, no dependencies beyond the standard library and OS headers. Linux/macOS/Windows on x86-64 and AArch64.
   - mimalloc-style segments (2 MiB) → pages (64 KiB) → size classes, with N × M thread-local heaps, one per (tier, lifetime) pair (default N = 3 tiers hot/warm/cold, M = 2 lifetimes transient/persistent).
   - Hint API: `tier::hint`, `tier::hint_for<T>` trait, `tier::managed<T>` CRTP base, `tier::make<T, H>`, `tier::allocator<T>`, `tier::alloc<H>(n)`. C++26 reflection annotations are used when `__cpp_impl_reflection` is defined.
   - Lifetime-aware free policy: transient pages are bump-only and reset when empty; persistent pages use LIFO free lists. Per-tier purge policy: `never` / `eager` / `decay(ms)`.
   - Optional `LD_PRELOAD` malloc replacement (Linux only).
2. **`tieralloc-trace` + `tieralloc-viz`**: Linux-only profiler. Inline ~40 B event records to thread-local rings; a single watcher thread handles draining, `/proc` sampling, `mincore`, and PEBS (`perf_event_open`). `tieralloc-viz` (Python) replays traces offline into views such as the address-space strip, live vs. RSS, hotness heatmap, hot-set span, page death spread, and hint audit.
3. **`tieralloc-bench`**: three workloads (fragmentation churn, order-book replay, pointer chase) across seven configurations (glibc, jemalloc, mimalloc, tieralloc without and with hints, hand-written pools, hinted tcmalloc). Performance results are Linux only; library tests run in CI on all three OSes.

## Key principles

- Hints are **user-declared, never predicted**. The profiler's job is to make wrong hints visible.
- `free` is tier-agnostic: it finds the segment by masking the pointer.
- The profiler's inline path does no syscalls, locks or allocation. The watcher never calls `malloc`. Rings block on overflow instead of dropping events.
- Timing runs are untraced; traced runs are never used for timing.
- Non-goals: multi-threaded scalability tuning, security hardening, lifetime prediction, drop-in override and profiler on macOS/Windows, interactive GUI. Anything on the out-of-scope list needs an explicit decision before it's added.

## Milestones

M0 harness and baselines → M1 profiler core → M2 allocator, drop-in on Linux → M3 compile-time hints → M4 hotness (PEBS) and snapshot mode → M5 free policies and published report. Future extensions start only after M5.
