#pragma once

#include <cstddef>
#include <cstdint>

namespace mvdb {

// L2 distance kernels over float32 vectors.
//
// `l2_rank` accumulates in float32: fast, used for graph traversal where only
// the ordering matters. `l2_exact` accumulates in double and is used for the
// distances that reach the user, so printed values do not drift with SIMD.
float l2_rank(const float* a, const float* b, size_t n);
double l2_exact(const float* a, const float* b, size_t n);

}  // namespace mvdb
