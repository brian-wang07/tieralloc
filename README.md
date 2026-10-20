# tieralloc
tieralloc is a cross-platform single-header C++ allocator library that places objects by compile-time tier and lifetime class, that ships with an in-depth runtime memory profiler for Linux, and a concrete benchmarking suite comparing tieralloc against other industry alternatives, such as glibc, jemalloc, mimalloc, hinted tcmalloc and hand-written pools.
**hypothesis**: Knowing at compile time which objects are hot and their relative lifetimes can help in performance in two domains:
1. Packing hot objects densely reduces the hot page's footprint and dTLB misses.
2. Segregating by lifetime ensures that entire pages are emptied together, which lowers RSS and heap fragmentation.
# Progress:
- Harness and Baselines (WIP)
- Heap Profiler
- Single-header `tieralloc.hpp`
- Compile-time hints
- PEBS sampling
- Free policies
- Interactive profiler gui
- Physical addresses
- `tier::alloc_near(p)`
- Write-up
## Details
|Term|Meaning|
|:---:|:---:|
|Tier *k*| Lower index means hotter|
|Lifetime class|transient: arena-style bump allocation. persistent: free-list slot reuse.|
|Heap|One heap per (tier, lifetime) pair|
|Page depth spread| Time between first and last free of objects on a given page: low spread means that a page empties at once|
|Fragmentation Ratio|RSS/live requested bytes|
## Requirements
tieralloc: C++20
tieralloc-bench: C++23+ (concepts) - prefer C++26 to use contract assertions
Existing allocators can be built with scripts/build-allocators.sh.
# tieralloc-bench
- The bench is built from 3 components: a workload source, a seam, and a harness.
- A workload to be benchmarked never invokes `new` or `malloc`, and are instead dispatched by the seam.
- Each allocation carries a role: one of Default, Hot, Transient, or Cold. These are standins for the hints that tieralloc will eventually take.
- Roles are defined as types and are templated, thus each call compiles to one direct path with no branching.
- The seam takes allocation and benchmarks them against each allocator configuration.
- Roles are currently dispatched to one of two backends:
  - plain ignores roles and just calls `operator new`: whatever allocator is loaded is responsible.
  - pools sends Hot objects to dense pages, transient objects to a bump arena and everything else to a fallback. This mirrors how tieralloc will function, but must be hand-tuned.
- The seam can essentially be thought of as abstracting away the exact allocator/pool to be used, and can easily be swapped out:
### example:
```cpp
struct Order { std::uint64_t id, price, qty, ts; };
// Roles can explicitly label types
Order *o1 = make<Order, Role::Cold>(...);
// the roles in a make/destroy pair must match; these are caught at runtime when contract assertions are enabled.
destroy<Order, Role::Cold>(o1);
```
- When run on the plain backend:
- `make` dispatches to the named overload:
```cpp
template <Makeable T, Role R = roleOf<std::remove_cv_t<T>>, class ... Args>
T *make(Args &&...args)
```
- and `R` is resolved as `Role::Cold` and `Args` is deduced from the arguments.
- `Makeable<T>` is a concept that checks if `T` is an object type, not an array and not abstract. 
- All of this is done at compile time; roles are invisible at runtime.
- This dispatches to `PlainBackend::allocate`, and alignment/size preconditions are checked. 
- The backend then calls `::operator new`, which gets emitted as symbol `_Znwm`. The dynamic linker then dispatches this to the preloaded allocator, and fallsback to glibc if none is supplied.
- The seam then records some stats: liveBytes, totalLiveBytes, number of allocations, and peakLiveBytes. this is the only overhead added at runtime.
- A similar process is done with `destroy`.

```cpp
// A type's role can be declared once, and inferred for subsequent uses.
template <> inline constexpr Role tabench::alloc::roleOf<Order> = Role::Hot;
Order *o2 = make<Order>(1, 100, 5, 0);
destroy(o2);

// containers can inherit roles
template <class T, tabench::alloc::Role R = tabench::alloc::roleOf<std::remove_cv_t<T>>>
using Vector = std::vector<T, tabench::alloc::Allocator<T, R>>;
Vector<Order *, Role::Hot> book;
// the allocator itself is empty as well.
static_assert(sizeof(Vector<Order *, Role::Hot>) == sizeof(std::vector<Order *>));
```
|Switch|Mechanism|Picks|
|:---:|:---:|:---:|
|Backend|Compile time: `TA_BACKEND_PLAIN` or `TA_BACKEND_POOLS` which build into seperate binaries|Who interprets roles|
|Allocator|Dynamically linked with `LD_PRELOAD`|Who serves `operator new`|
- The seam also verifies correctness and performs some simple bookkeeping (live bytes per role, allocation/free counts, and peak size)
- The harness is responsible for reporting concrete numbers. 
- Measurements for the harness comes from a Region. Each region measures (from begin->end)
  - Time elapsed, from the CPU's fixed-rate counter (TSC on x86 or `cntvct` on ARM)
  - Hardware counters: cycles, instructions, L1D, LLC, dTLB walks from a single perf group. These are read from outside of the timed window so syscall overhead is not counted.
  - Memory usage from `/proc`.
  - The CPU that ran the program is also recorded at begin and end, to ensure that unpinned runs that changed cpus is noted.
- All metrics are serialized into json.
# interesting stuff to read about 
- x86 CPUID leaves
- Get better at TMP



