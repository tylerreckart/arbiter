#pragma once
// Pure helpers for weighted split sizing. Extracted so unit tests can cover
// the available < N / rounding cases without constructing a LayoutTree.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace arbiter {

// Allocate `available` cells across `weights.size()` children by weight.
// Guarantees sum(result) == max(0, available). Individual sizes may be 0
// when available < N (callers that need a UI minimum must refuse the split
// earlier or clamp the outer bounds).
inline std::vector<int> allocate_weighted_sizes(int available,
                                                const std::vector<double>& weights) {
    const int N = static_cast<int>(weights.size());
    std::vector<int> sizes(static_cast<size_t>(std::max(0, N)), 0);
    if (N <= 0) return sizes;
    available = std::max(0, available);

    double weight_sum = 0.0;
    for (double w : weights) weight_sum += std::max(0.0001, w);
    if (weight_sum <= 0.0) weight_sum = static_cast<double>(N);

    // Largest-remainder method: floor shares, then hand leftover cells to
    // the largest fractional parts so the sum equals `available` exactly.
    // leftover is always in [0, N), so each child gets at most one extra.
    std::vector<double> frac(static_cast<size_t>(N), 0.0);
    int assigned = 0;
    for (int i = 0; i < N; ++i) {
        const double w = std::max(0.0001, weights[static_cast<size_t>(i)]);
        const double exact = (available * w) / weight_sum;
        int size = static_cast<int>(std::floor(exact));
        if (size < 0) size = 0;
        sizes[static_cast<size_t>(i)] = size;
        frac[static_cast<size_t>(i)] = exact - static_cast<double>(size);
        assigned += size;
    }

    int leftover = available - assigned;
    while (leftover > 0) {
        int best = -1;
        double best_frac = -1.0;
        for (int i = 0; i < N; ++i) {
            if (frac[static_cast<size_t>(i)] > best_frac) {
                best_frac = frac[static_cast<size_t>(i)];
                best = i;
            }
        }
        if (best < 0) break;
        sizes[static_cast<size_t>(best)] += 1;
        frac[static_cast<size_t>(best)] = -1.0;  // already received its remainder
        --leftover;
    }

    return sizes;
}

}  // namespace arbiter
