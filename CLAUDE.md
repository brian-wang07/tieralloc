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
- **Language standard:** C++26 for the bench, profiler and tooling, built with GCC 16 (one compiler for everything measured; Clang/LLVM for tooling and sanitizers only). `tieralloc.hpp` must stay C++20-compatible (its tests build at C++20).
- **Headers:** `#pragma once`. Includes go project headers first, then standard library headers, then platform headers guarded by `#if`.
- **Comments:** `//`, written as sentences; explain *why*, not what.
- **Contracts:** use the macros in `bench/harness/contracts.hpp`, never raw `pre`/`post`/`contract_assert`, so Clang (clangd, sanitizer builds) still parses. `TA_PRE(cond)` and `TA_POST(r, cond)` go on declarations for interface rules; `TA_ASSERT(cond)` goes in bodies. Predicates must be cheap and side-effect free; checks that write state stay manual under `TA_SEAM_CHECKS`. Value parameters used in a `TA_POST` must be `const`. Put contracts at the seam boundary, not in each backend. Timing builds use the `ignore` semantic (`TIERALLOC_CONTRACT_SEMANTIC`); tests use `enforce`. No contracts in `tieralloc.hpp`.

## Project

A hotness- and lifetime-aware C++ allocator, plus a memory profiler and a benchmark suite to evaluate it. Solo project; the full design doc lives outside the repo (`~/Downloads/tieralloc.md`).

## Hypothesis

Compile-time hints about which objects are **hot** and which **die together** buy measurable wins:
- Packing hot objects densely shrinks the hot set's page/cache-line footprint and cuts dTLB misses.
- Segregating by lifetime makes pages empty together, lowering RSS and fragmentation.

Raw malloc/free cost is expected to be near mimalloc's, not better. The headline comparison is **tieralloc with hints vs. tieralloc without hints** on the same workload; external allocators are context.

## Why not hinted tcmalloc + std::pmr

That combination is the closest existing alternative. tieralloc differs in four ways:

- **Compile-time hints.** Placement comes from the type (`tier::hint_for<T>`, `tier::managed<T>`, and C++26 reflection annotations once `__cpp_impl_reflection` is available), so the heap is fixed at compile time with no runtime lookup. Call sites don't pass hints and no `memory_resource` is threaded through. Hints are still declared by the user, never predicted.
- **N tiers.** tcmalloc's hot/cold `operator new` hint is binary. tieralloc has N tiers (default 3: hot/warm/cold) crossed with M lifetime classes.
- **Zero size overhead.** `std::pmr` stores a `memory_resource *` in every container and goes through a virtual `do_allocate`/`do_deallocate` on every call. `tier::allocator<T>` is stateless (empty) and dispatches statically. `free` finds the segment by masking the pointer, so objects carry no header and the pointer isn't stored anywhere.
- **Free policies that know the lifetime class.** pmr gives a choice between `monotonic_buffer_resource` (never frees) and pool resources (free lists). tieralloc ties the free policy to the lifetime class: transient pages are bump-only and reset when they empty, persistent pages use LIFO free lists. Each tier also has its own purge policy (`never` / `eager` / `decay(ms)`).

## Deliverables

1. **`tieralloc.hpp`**: stb-style single-header library (`#define TIERALLOC_IMPLEMENTATION` in one TU). C++20 minimum, no dependencies beyond the standard library and OS headers. Linux/macOS/Windows on x86-64 and AArch64.
   - mimalloc-style segments (2 MiB) → pages (64 KiB) → size classes, with N × M thread-local heaps, one per (tier, lifetime) pair (default N = 3 tiers hot/warm/cold, M = 2 lifetimes transient/persistent).
   - Hint API: `tier::hint`, `tier::hint_for<T>` trait, `tier::managed<T>` CRTP base, `tier::make<T, H>`, `tier::allocator<T>`, `tier::alloc<H>(n)`. C++26 reflection annotations are used when `__cpp_impl_reflection` is defined.
   - Lifetime-aware free policy: transient pages are bump-only and reset when empty; persistent pages use LIFO free lists. Per-tier purge policy: `never` / `eager` / `decay(ms)`.
   - Optional `LD_PRELOAD` malloc replacement (Linux only).
2. **`tieralloc-trace` + `tieralloc-viz`**: Linux-only profiler. Inline ~40 B event records to thread-local rings; a single watcher thread handles draining, `/proc` sampling, `mincore`, and PEBS (`perf_event_open`). `tieralloc-viz` (Python) replays traces offline into views such as the address-space strip, live vs. RSS, hotness heatmap, hot-set span, page death spread, and hint audit. It must also cover every feature of the positional heap profiler described under *Positional heap views* below.
3. **`tieralloc-bench`**: three workloads (fragmentation churn, order-book replay, pointer chase) across seven configurations (glibc, jemalloc, mimalloc, tieralloc without and with hints, hand-written pools, hinted tcmalloc). Performance results are Linux only; library tests run in CI on all three OSes.

## Positional heap views

These are modeled on the "Memory Allocation Analyzer" positional heap profiler from a CppCon 2026 talk ("Let's Build a Positional Heap Profiler: Visualizing Pinning"). `tieralloc-viz` must support all of its features, for both glibc traces and tieralloc traces. The point is to show **pinning**: memory that stays resident because a few live allocations are scattered across otherwise empty heaps.

- **Summary bar.** Total allocations, active (live) allocations, and heap count.
- **Memory over time.** A line chart of total heap size (RSS owned by the heap), total allocated (live) bytes, and mmap'd allocations. A click selects a time point, and back/forward buttons step through time (5 s by default). There's a fullscreen mode. Every other view shows state at the selected time.
- **Tabs.** Arenas / Heatmaps, Size Distribution, and Flamegraphs, all for the selected time.
- **Arenas table.** One row per arena with its ID and label (e.g. `main`, `mmap'd`), heap count, total heap size, and total allocated. Selecting a row opens that arena's heatmaps.
- **Per-heap heatmaps.** One strip per heap in the selected arena. Each strip is a grid of fixed-size address-range blocks (e.g. 512 blocks laid out 128×4), colored by occupancy density from 0% to 100%. The strip header shows the heap base address, size, allocation count, used bytes with the used percentage, and the block grid shape. The unused tail of the last heap (glibc's top chunk) is drawn as a separate, visually distinct region.
- **Block allocation details.** Clicking a block lists the live allocations in its address range: index, address, size, allocation callstack, and free callstack (`N/A` while the allocation is still live). The header shows the heap, the block index and address range, and the allocation count.

How these map onto tieralloc: an arena is a (tier, lifetime) heap (per thread), a heap is a 2 MiB segment, and the heatmap grid can be drawn at page (64 KiB) or finer granularity. The top chunk corresponds to the not-yet-bumped tail of a page or segment. To produce the block details view, the trace has to record callstack IDs (deduplicated, resolved offline) on both alloc and free. That has to fit the inline-path constraints below.

## Key principles

- Hints are **user-declared, never predicted**. The profiler's job is to make wrong hints visible.
- `free` is tier-agnostic: it finds the segment by masking the pointer.
- The profiler's inline path does no syscalls, locks or allocation. The watcher never calls `malloc`. Rings block on overflow instead of dropping events.
- Timing runs are untraced; traced runs are never used for timing.
- Non-goals: multi-threaded scalability tuning, security hardening, lifetime prediction, drop-in override and profiler on macOS/Windows, a live or attach-to-process GUI. Interactive viz output, like the positional heap views, is in scope only as an offline report generated from a recorded trace. Anything on the out-of-scope list needs an explicit decision before it's added.

## Milestones

M0 harness and baselines → M1 profiler core → M2 allocator, drop-in on Linux → M3 compile-time hints → M4 hotness (PEBS) and snapshot mode → M5 free policies and published report. Future extensions start only after M5.
