#include "vector/distance.h"

#include <cmath>
#include <immintrin.h>

namespace mvdb {

namespace {

float l2_rank_scalar(const float* a, const float* b, size_t n) {
  float sum = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const float diff = a[i] - b[i];
    sum += diff * diff;
  }
  return std::sqrt(sum);
}

double l2_exact_scalar(const float* a, const float* b, size_t n) {
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += diff * diff;
  }
  return std::sqrt(sum);
}

__attribute__((target("avx2,fma"))) float l2_rank_avx2(const float* a, const float* b, size_t n) {
  __m256 acc = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256 va = _mm256_loadu_ps(a + i);
    const __m256 vb = _mm256_loadu_ps(b + i);
    const __m256 diff = _mm256_sub_ps(va, vb);
    acc = _mm256_fmadd_ps(diff, diff, acc);
  }
  __m128 low = _mm256_castps256_ps128(acc);
  __m128 high = _mm256_extractf128_ps(acc, 1);
  __m128 sum = _mm_add_ps(low, high);
  sum = _mm_hadd_ps(sum, sum);
  sum = _mm_hadd_ps(sum, sum);
  float total = _mm_cvtss_f32(sum);
  for (; i < n; ++i) {
    const float diff = a[i] - b[i];
    total += diff * diff;
  }
  return std::sqrt(total);
}

__attribute__((target("avx2,fma"))) double l2_exact_avx2(const float* a, const float* b, size_t n) {
  __m256d acc0 = _mm256_setzero_pd();
  __m256d acc1 = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256 va = _mm256_loadu_ps(a + i);
    const __m256 vb = _mm256_loadu_ps(b + i);
    const __m256 diff = _mm256_sub_ps(va, vb);
    const __m256d lo = _mm256_cvtps_pd(_mm256_castps256_ps128(diff));
    const __m256d hi = _mm256_cvtps_pd(_mm256_extractf128_ps(diff, 1));
    acc0 = _mm256_fmadd_pd(lo, lo, acc0);
    acc1 = _mm256_fmadd_pd(hi, hi, acc1);
  }
  double total = 0.0;
  double lanes[4];
  _mm256_storeu_pd(lanes, _mm256_add_pd(acc0, acc1));
  total = lanes[0] + lanes[1] + lanes[2] + lanes[3];
  for (; i < n; ++i) {
    const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    total += diff * diff;
  }
  return std::sqrt(total);
}

bool has_avx2() {
  static const bool available = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  return available;
}

}  // namespace

float l2_rank(const float* a, const float* b, size_t n) {
  return has_avx2() ? l2_rank_avx2(a, b, n) : l2_rank_scalar(a, b, n);
}

double l2_exact(const float* a, const float* b, size_t n) {
  return has_avx2() ? l2_exact_avx2(a, b, n) : l2_exact_scalar(a, b, n);
}

}  // namespace mvdb
