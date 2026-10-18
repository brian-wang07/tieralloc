#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace tabench::alloc {

enum class Role : std::uint8_t {
  Default,
  Hot,
  // lifetime specifier
  Transient,
  // keep cold as last variant
  Cold
};

constexpr std::size_t index(Role role) {
  return static_cast<std::size_t>(std::to_underlying(role));
}

inline constexpr std::size_t roleCount = 4;
static_assert(index(Role::Cold) + 1 == roleCount);

constexpr const char *toString(Role role) {
  switch (role) {
  case Role::Default:
    return "default";
  case Role::Hot:
    return "hot";
  case Role::Transient:
    return "transient";
  case Role::Cold:
    return "cold";
  }
  std::unreachable();
}

template <class T> inline constexpr Role roleOf = Role::Default;

} // namespace tabench::alloc
