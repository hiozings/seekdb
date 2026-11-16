#pragma once

#include <cstddef>
#include <cstdint>

namespace mvdb {

// L2 distance kernels over float32 vectors.
//
// Both ranking and output widen before subtracting and accumulate in double.
// Finite float32 coordinates can overflow or underflow float32 squared distances,
// destroying graph navigation even if the final candidates are re-ranked exactly.
double l2_rank(const float* a, const float* b, size_t n);
double l2_exact(const float* a, const float* b, size_t n);

}  // namespace mvdb
