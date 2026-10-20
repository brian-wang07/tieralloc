#pragma once

// What a workload may say about an allocation: a role, the bench's stand-in
// for tieralloc's hints. Backends map roles to placement; workloads never know
// which backend runs. A role is fixed at compile time (roleOf<T>, or an
// explicit R) so each call compiles to one direct path; withRole() turns a
// role known only at run time into that compile-time form.

#include "ta/contracts.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

namespace tabench::alloc {

// Each role is a claim the workload author makes, which the profiler can later
// show to be wrong. Default is 0 so a zero-initialized role byte (as stored on
// an object whose role is chosen at run time) means "no claim".
enum class Role : std::uint8_t {
  Default, // No claim.
  Hot,     // Touched often inside the timed region; pack densely.
  // Lifetime, not hotness: freed before the next endPhase().
  Transient,
  // Rarely touched after construction. Keep it last: the static_assert on
  // roleCount relies on it, so new roles go before Cold.
  Cold
};

// A role as a type: an empty value whose type carries R. withRole() passes one
// to its callback, which can use it as a template argument, e.g. make<T, r>().
template <Role R> using RoleTag = std::integral_constant<Role, R>;

constexpr std::size_t index(Role role) noexcept {
  return std::to_underlying(role);
}

inline constexpr std::size_t roleCount = 4;
static_assert(index(Role::Cold) + 1 == roleCount);

// JSON keys in every result record: renaming one changes the output format.
inline constexpr std::array<const char *, roleCount> roleNames{
    "default", "hot", "transient", "cold"};

constexpr const char *toString(Role role) noexcept
    TA_PRE(index(role) < roleCount) {
  return roleNames[index(role)];
}

namespace detail {

// A name too many fails to compile, but a missing one silently becomes
// nullptr, and a copied one gives duplicate JSON keys.
consteval bool validRoleNames() {
  for (std::size_t i = 0; i < roleCount; ++i) {
    if (!roleNames[i] || *roleNames[i] == '\0')
      return false;
    for (std::size_t j = i + 1; j < roleCount; ++j)
      if (std::string_view(roleNames[i]) == roleNames[j])
        return false;
  }
  return true;
}

// Compares role against I, I+1, ... and calls f with the matching tag. The
// last role needs no comparison: withRole's range check leaves no other
// possibility, and under the ignore semantic a corrupted role then picks a
// wrong role instead of undefined behavior.
template <std::size_t I, class F>
constexpr decltype(auto) withRoleFrom(Role role, F &f) {
  if constexpr (I + 1 == roleCount) {
    return f(RoleTag<static_cast<Role>(I)>{});
  } else {
    if (index(role) == I)
      return f(RoleTag<static_cast<Role>(I)>{});
    return withRoleFrom<I + 1>(role, f);
  }
}

} // namespace detail

static_assert(detail::validRoleNames(),
              "roleNames needs one unique, non-empty name per Role");

// Calls f(RoleTag<R>{}) with R == role and returns its result. f must accept a
// RoleTag of every role (a lambda taking auto does) and return the same type
// for all of them.
//
// For mechanical dispatch only. Decisions about what a role means (pools
// routing, tieralloc's tier mapping) stay explicit switches with no default,
// so -Werror=switch forces a decision whenever a role is added.
//
// The range check is a TA_ASSERT rather than a TA_PRE: GCC 16.2 crashes
// (internal compiler error in check_noexcept_r) on a precondition on this
// constexpr function template when it is called with a generic lambda.
template <class F> constexpr decltype(auto) withRole(Role role, F &&f) {
  TA_ASSERT(index(role) < roleCount);
  return detail::withRoleFrom<0>(role, f);
}

// The role a type gets when the call site does not name one. Specialize it
// right after the type, before the first make<T>(), with a qualified name from
// an enclosing namespace (here, namespace tabench):
//   template <> inline constexpr Role alloc::roleOf<Order> = Role::Hot;
// A translation unit that does not see the specialization silently uses
// Default, and frees then disagree with allocations.
template <class T> inline constexpr Role roleOf = Role::Default;

} // namespace tabench::alloc
