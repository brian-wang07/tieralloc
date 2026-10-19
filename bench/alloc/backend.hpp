#pragma once

// The interface between the seam and its backends. A leaf header: it includes
// neither the seam nor any backend, so each backend can include it and check
// its own conformance next to its definition, and the seam can rely on it.

#include "role.hpp"

#include <nlohmann/json.hpp>

#include <concepts>
#include <cstddef>
#include <string_view>

namespace tabench::alloc {

// What every backend provides. Member templates are checked with one role;
// backends are not expected to treat roles differently in their signatures.
template <class B>
concept SeamBackend = requires(void *p, std::size_t n) {
  { B::name } -> std::convertible_to<std::string_view>;
  { B::template allocate<Role::Default>(n, n) } -> std::same_as<void *>;
  { B::template deallocate<Role::Default>(p, n, n) } noexcept;
  { B::endPhase() } noexcept;
  { B::extraStats() } -> std::same_as<nlohmann::ordered_json>;
};

// Optional hooks: pure, side-effect-free checks the seam's contracts call, so
// a backend can say whether a pointer is valid for a request without holding
// contracts of its own. A backend without them (plain) is simply not checked.
//
// A hook whose signature differs at all (not noexcept, void * instead of
// const void *, another name) makes these concepts false and silently turns
// the checks off, so a backend that has hooks must static_assert them.
template <class B, Role R>
concept ChecksAllocations = requires(const void *p, std::size_t n) {
  { B::template validAllocation<R>(p, n, n) } noexcept -> std::same_as<bool>;
};

template <class B, Role R>
concept ChecksFrees = requires(const void *p, std::size_t n) {
  { B::template validFree<R>(p, n, n) } noexcept -> std::same_as<bool>;
};

} // namespace tabench::alloc
