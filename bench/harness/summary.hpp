#pragma once

// Order statistics over the samples of one run

#include "contracts.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace tabench {

struct Summary {
  std::size_t count;
  double min;
  double p50;
  double p99;
  double max;
  double mean;
};

// q-quantile of sorted (ascending) values, interpolating linearly between
// the two nearest ranks
[[nodiscard]] inline double percentile(std::span<const double> sorted,
                                       double q) noexcept
    TA_PRE(!sorted.empty()) TA_PRE(q >= 0.0 && q <= 1.0) {
  const double rank = q * static_cast<double>(sorted.size() - 1);
  const auto below = static_cast<std::size_t>(rank);
  if (below + 1 >= sorted.size())
    return sorted.back();
  return std::lerp(sorted[below], sorted[below + 1],
                   rank - static_cast<double>(below));
}

[[nodiscard]] inline Summary summarize(std::span<double> values) noexcept
    TA_PRE(!values.empty()) {
  std::ranges::sort(values);
  double sum = 0.0;
  for (double v : values)
    sum += v;
  return {
      .count = values.size(),
      .min = values.front(),
      .p50 = percentile(values, 0.50),
      .p99 = percentile(values, 0.99),
      .max = values.back(),
      .mean = sum / static_cast<double>(values.size()),
  };
}

} // namespace tabench
