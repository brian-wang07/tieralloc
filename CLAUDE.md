# tieralloc

## Working with me

- **Don't write the implementation code unless I explicitly ask for it.** By default, give an outline of what needs to be done: components, responsibilities, interfaces, ordering, pitfalls. Small illustrative snippets are fine only when prose would be unclear.
- Build/config scaffolding (e.g. CMake) is fine to write when I ask for it.
- **Always bound parallel builds**: `cmake --build build -j8`, never a bare `-j` (or `make -j` / `ninja` with no limit). An unbounded `-j` spawns a compiler per target at once and can exhaust memory and crash the machine.

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
  - Internal helpers go in a nested `detail` namespace in headers, or an anonymous namespace in `.cpp` files; no underscore prefixes.
- **Language standard:** C++26 for the bench, profiler and tooling, built with GCC 16 (one compiler for everything measured; Clang/LLVM for tooling and sanitizers only). `tieralloc.hpp` must stay C++20-compatible (its tests build at C++20).
- **Headers:** `#pragma once`. Includes go project headers first, then standard library headers, then platform headers guarded by `#if`.
- **Comments:** `//`, written as sentences; explain *why*, not what.
- **Contracts:** use the macros in `common/ta/contracts.hpp`, never raw `pre`/`post`/`contract_assert`, so Clang (clangd, sanitizer builds) still parses. `TA_PRE(cond)` and `TA_POST(r, cond)` go on declarations for interface rules; `TA_ASSERT(cond)` goes in bodies. Predicates must be cheap and side-effect free; checks that write state stay manual under `TA_SEAM_CHECKS`. Value parameters used in a `TA_POST` must be `const`. Put contracts at the seam boundary, not in each backend. Timing builds use the `ignore` semantic (`TIERALLOC_CONTRACT_SEMANTIC`); tests use `enforce`. No contracts in `tieralloc.hpp`.

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
2. **`tieralloc-trace` + `tieralloc-viz`**: Linux-only profiler. An `LD_PRELOAD` shim writes inline ~40 B event records to per-thread rings in a shared-memory pool. A separate watcher process, which is also the launcher (`tieralloc-trace run -- <program>`), drains the rings into the trace file and does the `/proc` sampling (`smaps`, `pagemap`) and PEBS (`perf_event_open`) from outside the traced process. Tracing always goes through the launcher; the shim preloaded on its own is a plain pass-through. `tieralloc-viz` (Python) replays traces offline into views such as the address-space strip, live vs. RSS, hotness heatmap, hot-set span, page death spread, and hint audit. It must also cover every feature of the positional heap profiler described under *Positional heap views* below.
3. **`tieralloc-bench`**: three workloads (fragmentation churn, order-book replay, pointer chase) across seven configurations (glibc, jemalloc, mimalloc, tieralloc without and with hints, hand-written pools, hinted tcmalloc). Performance results are Linux only; library tests run in CI on all three OSes.

## Positional heap views

These are modeled on the "Memory Allocation Analyzer" positional heap profiler from a CppCon 2026 talk ("Let's Build a Positional Heap Profiler: Visualizing Pinning"). `tieralloc-viz` must support all of its features, for both glibc traces and tieralloc traces. The point is to show **pinning**: memory that stays resident because a few live allocations are scattered across otherwise empty heaps.

- **Summary bar.** Total allocations, active (live) allocations, and heap count.
- **Memory over time.** A line chart with one point per sample. The headline series are heap-resident bytes and live requested bytes, with the fragmentation ratio (resident / live) on a secondary axis. Heap extent (mapped size) and mmap'd allocations are secondary, drawn dashed. A click selects a time point, and back/forward buttons step through time (5 s by default; adaptive for short runs). There's a fullscreen mode. Every other view shows state at the selected time.
- **Live bytes by allocation-time cohort.** A stacked area chart next to memory over time. Each layer holds the bytes allocated in one time window that are still live, so the thin layers left after a mass free show which phases' survivors pin the heap.
- **Tabs.** Arenas / Heatmaps, Size Distribution, and Flamegraphs, all for the selected time.
- **Arenas table.** One row per arena with its ID and label (e.g. `main`, `mmap'd`), heap count, total heap size, and total allocated. Selecting a row opens that arena's heatmaps.
- **Per-heap heatmaps.** One strip per heap in the selected arena. Each strip is a grid of fixed-size address-range blocks (e.g. 512 blocks laid out 128×4), colored by occupancy density from 0% to 100%. The strip header shows the heap base address, size, allocation count, used bytes with the used percentage, and the block grid shape. The unused tail of the last heap (glibc's top chunk) is drawn as a separate, visually distinct region.
- **Block allocation details.** Clicking a block lists the live allocations in its address range: index, address, size, allocation callstack, and free callstack. The free callstack is where that allocation is eventually freed later in the trace; it is `N/A` only if the allocation is never freed before the trace ends (the pinning survivors). The header shows the heap, the block index and address range, and the allocation count. A block can hold thousands of allocations, so the list is paged.

**Headline memory metric.** Heap-resident bytes, not heap extent, is the headline, and it uses the bench's RSS definition (`Anonymous − LazyFree`). Extent is virtual: jemalloc and mimalloc keep reserved address space, and tieralloc's purge policies lower RSS without shrinking a segment, so extent would hide exactly the wins this project claims. Chart totals come from the traced process's per-mapping `/proc/<pid>/smaps` (`Rss − LazyFree` over the heap mappings). The ring pool is shared memory (`Shmem`, not `Anonymous`), so it never counts toward the headline. `/proc/<pid>/pagemap` present bits are used only for per-page residency in the heatmaps; like `mincore`, they show `MADV_FREE`'d pages as resident until reclaim, and the report notes this.

How these map onto glibc: an arena is identified by its `ar_ptr` (the main arena is labelled `main`, and mmap'd chunks form an `mmap'd` pseudo-arena), and a heap is a 64 MiB non-main heap or the brk region. Other external allocators (jemalloc, mimalloc, tcmalloc) use a generic layout: one arena, with heaps being the anonymous mappings that hold allocations.

How these map onto tieralloc: an arena is a (tier, lifetime) heap (per thread), a heap is a 2 MiB segment, and the heatmap grid can be drawn at page (64 KiB) or finer granularity. The top chunk corresponds to the not-yet-bumped tail of a page or segment. To produce the block details view, the trace has to record callstack IDs (deduplicated, resolved offline) on both alloc and free. That has to fit the inline-path constraints below.

**Profiler decisions (from comparing with heaptrack, heapprofd, bytehound, tcmalloc/jemalloc profiling, eBPF `memleak`, DHAT and Tracy).** These shape the record format, so they're settled before the drain and recording sub-steps.
- **Every allocation is recorded; nothing is sampled.** The positional views need every live allocation. The consumer stays out of process, so the target's heap isn't disturbed.
- **Callstacks:** the shim walks the frame-pointer chain (the build already uses `-fno-omit-frame-pointer`; Fedora's libraries have frame pointers), skipping its own frames. `stackId` is a 64-bit hash of the PCs, so threads need no coordination. Each ring has a small cache in the pool of the hashes its thread has already sent. The first time a thread sees a stack, it sends a multi-slot `Stack` record. The watcher deduplicates across threads, comparing the PCs when the same hash comes from two threads. Free stacks use the same mechanism.
- **Module map:** the watcher reads `/proc/<pid>/maps` and the build-ID of each executable mapping once the shim attaches, and writes both into the trace. The shim hooks `dlopen`/`dlclose` and writes a marker, so the watcher reads the maps again. A library opened and closed between two passes is missed; that's accepted.
- **Size:** chunks aren't compressed yet, but the chunk header reserves a "compressed" flag so zstd can be added later without a format bump.
- **Checks and exports:** a script, not a CTest, compares counts, peak live bytes and never-freed bytes against heaptrack on the bench workloads. The Flamegraphs tab exports folded stacks or pprof, so standard tools can check it.

## Key principles

- Hints are **user-declared, never predicted**. The profiler's job is to make wrong hints visible.
- `free` is tier-agnostic: it finds the segment by masking the pointer.
- The profiler's inline path (the shim's hooks) does no syscalls, locks or allocation. Rings are mapped once when the shim starts, and a thread's first event claims one from the pool with atomics only; the one-time `gettid` per thread is the only other syscall. Rings block on overflow instead of dropping events; the blocked producer spins, then falls back to `sched_yield`, and the stall time is recorded. That fallback is the one deliberate syscall exception, since it only runs when a ring is full. The watcher is a separate process, so it may allocate freely; if it stops responding, a blocked producer gives up and counts the dropped event rather than hanging the program.
- Timing runs are untraced; traced runs are never used for timing.
- Non-goals: multi-threaded scalability tuning, security hardening, lifetime prediction, drop-in override and profiler on macOS/Windows, a live or attach-to-process GUI. Interactive viz output, like the positional heap views, is in scope only as an offline report generated from a recorded trace. Anything on the out-of-scope list needs an explicit decision before it's added.

## Milestones

M0 harness and baselines → M1 profiler core → M2 allocator, drop-in on Linux → M3 compile-time hints → M4 hotness (PEBS) and snapshot mode → M5 free policies and published report. Future extensions start only after M5.
