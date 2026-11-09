// Quality check for the HNSW index in isolation: compares approximate
// results against brute force over the same vectors.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>

#include "vector/hnsw.h"

namespace {

std::vector<int64_t> brute_force(const std::vector<std::vector<float>>& data, const std::vector<float>& q,
                                 size_t k) {
  std::vector<std::pair<float, int64_t>> scored;
  scored.reserve(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    double sum = 0.0;
    for (size_t d = 0; d < q.size(); ++d) {
      const double diff = static_cast<double>(data[i][d]) - static_cast<double>(q[d]);
      sum += diff * diff;
    }
    scored.emplace_back(static_cast<float>(std::sqrt(sum)), static_cast<int64_t>(i + 1));
  }
  std::sort(scored.begin(), scored.end());
  std::vector<int64_t> out;
  for (size_t i = 0; i < k && i < scored.size(); ++i) out.push_back(scored[i].second);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const int kRows = argc > 1 ? std::atoi(argv[1]) : 5000;
  const int kDim = argc > 2 ? std::atoi(argv[2]) : 32;
  const int kQueries = argc > 3 ? std::atoi(argv[3]) : 100;
  constexpr size_t kK = 10;

  std::mt19937 rng(20261109);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<std::vector<float>> data(kRows, std::vector<float>(kDim));
  for (auto& row : data) {
    for (float& value : row) value = dist(rng);
  }

  const int m = argc > 4 ? std::atoi(argv[4]) : 16;
  const int efc = argc > 5 ? std::atoi(argv[5]) : 200;
  mvdb::VectorIndex index(kDim, m, efc, 42);
  for (int i = 0; i < kRows; ++i) {
    index.add(static_cast<int64_t>(i + 1), data[static_cast<size_t>(i)], {});
  }
  std::printf("index size = %zu\n", index.size());

  for (size_t ef : {16, 32, 64, 128, 256, 512, 1024}) {
    double total = 0.0;
    for (int q = 0; q < kQueries; ++q) {
      std::vector<float> query(kDim);
      for (float& value : query) value = dist(rng);
      const auto truth = brute_force(data, query, kK);
      const auto hits = index.search(query, kK, ef, nullptr);
      size_t found = 0;
      for (const auto& hit : hits) {
        if (std::find(truth.begin(), truth.end(), hit.pk) != truth.end()) ++found;
      }
      total += static_cast<double>(found) / static_cast<double>(kK);
    }
    std::printf("ef=%4zu recall@10 = %.4f\n", ef, total / kQueries);
  }

  // Tombstones must not leak into results.
  for (int i = 0; i < 1000; ++i) index.remove(static_cast<int64_t>(i + 1));
  std::vector<float> query(kDim);
  for (float& value : query) value = dist(rng);
  const auto alive = index.search(query, kK, 64, nullptr);
  bool leaked = false;
  for (const auto& hit : alive) {
    if (hit.pk >= 1 && hit.pk <= 1000) leaked = true;
  }
  std::printf("tombstone check: %s\n", leaked ? "LEAKED" : "ok");

  return 0;
}
