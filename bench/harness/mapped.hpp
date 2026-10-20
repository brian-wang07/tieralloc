#pragma once

// Memory a workload needs for itself (input tapes, its bookkeeping arrays),
// mapped directly so it never goes through the allocator under test. Every
// page is touched up front, so it is resident before the measured region and
// its size is exact: Session::addInputBytes(bytes()) lets the summary subtract
// it from the heap's RSS. Huge pages are refused for the same reason: under
// THP=always a 2 MiB page would make the resident size differ from bytes().

#include <cstddef>
#include <cstring>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

namespace tabench {

template <class T> class MappedArray {
  static_assert(std::is_trivially_copyable_v<T> &&
                    std::is_trivially_destructible_v<T>,
                "the mapping is zero-filled and unmapped without destructors");

public:
  MappedArray() noexcept = default;

  // count zero-initialized elements. Throws std::bad_alloc if mmap fails.
  explicit MappedArray(std::size_t count) : count(count) {
    if (count == 0)
      return;
    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    mapped = (count * sizeof(T) + page - 1) / page * page;
    void *p = ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
      throw std::bad_alloc();
#if defined(MADV_NOHUGEPAGE)
    ::madvise(p, mapped, MADV_NOHUGEPAGE);
#endif
    // A write per byte, not just MAP_POPULATE, so the pages are anonymous and
    // resident on every kernel; zero is also what T's value is meant to be.
    std::memset(p, 0, mapped);
    data = static_cast<T *>(p);
  }

  MappedArray(MappedArray &&other) noexcept
      : data(std::exchange(other.data, nullptr)),
        count(std::exchange(other.count, 0)),
        mapped(std::exchange(other.mapped, 0)) {}

  MappedArray &operator=(MappedArray &&other) noexcept {
    if (this != &other) {
      release();
      data = std::exchange(other.data, nullptr);
      count = std::exchange(other.count, 0);
      mapped = std::exchange(other.mapped, 0);
    }
    return *this;
  }

  MappedArray(const MappedArray &) = delete;
  MappedArray &operator=(const MappedArray &) = delete;

  ~MappedArray() { release(); }

  T &operator[](std::size_t i) noexcept { return data[i]; }
  const T &operator[](std::size_t i) const noexcept { return data[i]; }
  std::span<T> span() noexcept { return {data, count}; }
  std::span<const T> span() const noexcept { return {data, count}; }
  std::size_t size() const noexcept { return count; }

  // The resident size, page-rounded: what to pass to addInputBytes().
  std::size_t bytes() const noexcept { return mapped; }

private:
  void release() noexcept {
    if (data != nullptr)
      ::munmap(data, mapped);
    data = nullptr;
  }

  T *data = nullptr;
  std::size_t count = 0;
  std::size_t mapped = 0;
};

} // namespace tabench
