#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "sql/sql.h"

namespace mvdb {

// In-memory HNSW graph over float32 vectors, keyed by primary key.
//
// Deleted rows are tombstoned: they stay in the graph for navigation but are
// never returned. The engine rebuilds the index when the tombstone ratio grows
// past a threshold or when a rolled-back transaction may have left it stale.
class VectorIndex {
 public:
  using AcceptFn = std::function<bool(const std::vector<Value>&)>;

  struct Hit {
    int64_t pk = 0;
    float distance = 0.0f;
  };

  struct SearchStats {
    size_t visited = 0;
    size_t accepted = 0;
  };

  VectorIndex(int64_t dim, int m = 16, int ef_construction = 200, uint64_t seed = 1);

  void clear();
  void add(int64_t pk, const std::vector<float>& vec, std::vector<Value> row);
  void remove(int64_t pk);

  size_t size() const { return current_.size(); }
  size_t tombstones() const { return tombstones_; }
  int64_t dim() const { return dim_; }

  // Returns up to `k` accepted hits ordered by distance. Fewer than `k` means
  // the search did not cover enough of the filtered set; the caller escalates
  // `ef` instead of accepting the short answer.
  std::vector<Hit> search(const std::vector<float>& query, size_t k, size_t ef,
                          const AcceptFn& accept, SearchStats* stats = nullptr) const;

 private:
  struct Node {
    std::vector<float> vec;
    std::vector<Value> row;
    std::vector<std::vector<int>> links;
    int64_t pk = 0;
    bool deleted = false;
  };

  int random_level();
  int greedy_closest(const float* query, int entry, int level) const;
  std::vector<std::pair<float, int>> search_layer(const float* query, int entry, size_t ef,
                                                  int level) const;
  std::vector<int> select_neighbors(const std::vector<std::pair<float, int>>& candidates,
                                    size_t limit) const;
  void connect(int node, int neighbor, int level);

  int64_t dim_;
  int m_;
  int ef_construction_;
  uint64_t rng_state_;

  std::vector<Node> nodes_;
  std::unordered_map<int64_t, int> current_;
  size_t tombstones_ = 0;
  int entry_ = -1;
  int max_level_ = 0;

  mutable std::vector<uint32_t> visit_stamp_;
  mutable uint32_t visit_epoch_ = 0;
};

}  // namespace mvdb
