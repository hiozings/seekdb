#include "vector/distance.h"

#include <cmath>
#include <cstring>
#include <immintrin.h>

namespace mvdb {

namespace {

float load_float(const float* data, size_t index) {
  // Encoded heap rows are byte-packed, so scalar tails need unaligned loads
  // just like the SIMD path. memcpy also avoids typed dereferences of row bytes.
  float value;
  std::memcpy(&value, reinterpret_cast<const char*>(data) + index * sizeof(float), sizeof(value));
  return value;
}

double l2_exact_scalar(const float* a, const float* b, size_t n) {
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double diff = static_cast<double>(load_float(a, i)) - static_cast<double>(load_float(b, i));
    sum += diff * diff;
  }
  return std::sqrt(sum);
}

__attribute__((target("avx2,fma"))) double l2_exact_avx2(const float* a, const float* b, size_t n) {
  __m256d acc0 = _mm256_setzero_pd();
  __m256d acc1 = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256 va = _mm256_loadu_ps(a + i);
    const __m256 vb = _mm256_loadu_ps(b + i);
    // Widen before subtraction: two finite float32 values can have a
    // difference outside float32 range, even though it fits in double.
    const __m256d lo = _mm256_sub_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(va)),
                                    _mm256_cvtps_pd(_mm256_castps256_ps128(vb)));
    const __m256d hi = _mm256_sub_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(va, 1)),
                                    _mm256_cvtps_pd(_mm256_extractf128_ps(vb, 1)));
    acc0 = _mm256_fmadd_pd(lo, lo, acc0);
    acc1 = _mm256_fmadd_pd(hi, hi, acc1);
  }
  double total = 0.0;
  double lanes[4];
  _mm256_storeu_pd(lanes, _mm256_add_pd(acc0, acc1));
  total = lanes[0] + lanes[1] + lanes[2] + lanes[3];
  for (; i < n; ++i) {
    const double diff = static_cast<double>(load_float(a, i)) - static_cast<double>(load_float(b, i));
    total += diff * diff;
  }
  return std::sqrt(total);
}

bool has_avx2() {
  static const bool available = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  return available;
}

}  // namespace

double l2_rank(const float* a, const float* b, size_t n) {
  return l2_exact(a, b, n);
}

double l2_exact(const float* a, const float* b, size_t n) {
  return has_avx2() ? l2_exact_avx2(a, b, n) : l2_exact_scalar(a, b, n);
}

}  // namespace mvdb
