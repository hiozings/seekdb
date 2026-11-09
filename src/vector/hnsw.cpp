#include "vector/hnsw.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <utility>

namespace mvdb {

namespace {

constexpr int kMaxLevel = 16;

float l2_distance(const float* a, const float* b, size_t dim) {
  double sum = 0.0;
  for (size_t i = 0; i < dim; ++i) {
    const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += diff * diff;
  }
  return static_cast<float>(std::sqrt(sum));
}

}  // namespace

VectorIndex::VectorIndex(int64_t dim, int m, int ef_construction, uint64_t seed)
    : dim_(dim), m_(m), ef_construction_(ef_construction), rng_state_(seed) {}

void VectorIndex::clear() {
  nodes_.clear();
  current_.clear();
  tombstones_ = 0;
  entry_ = -1;
  max_level_ = 0;
  visit_stamp_.clear();
}

int VectorIndex::random_level() {
  // xorshift64*, deterministic for a given seed so rebuilds match.
  rng_state_ ^= rng_state_ >> 12;
  rng_state_ ^= rng_state_ << 25;
  rng_state_ ^= rng_state_ >> 27;
  const uint64_t x = rng_state_ * 0x2545F4914F6CDD1DULL;
  const double u = static_cast<double>((x >> 11) + 1) / static_cast<double>(1ULL << 53);
  const double scale = 1.0 / std::log(static_cast<double>(m_));
  int level = static_cast<int>(-std::log(u) * scale);
  if (level < 0) level = 0;
  if (level > kMaxLevel) level = kMaxLevel;
  return level;
}

int VectorIndex::greedy_closest(const float* query, int entry, int level) const {
  int best = entry;
  float best_dist = l2_distance(query, nodes_[best].vec.data(), static_cast<size_t>(dim_));
  bool improved = true;
  while (improved) {
    improved = false;
    if (level >= static_cast<int>(nodes_[best].links.size())) break;
    for (int neighbor : nodes_[best].links[static_cast<size_t>(level)]) {
      const float dist = l2_distance(query, nodes_[neighbor].vec.data(), static_cast<size_t>(dim_));
      if (dist < best_dist) {
        best_dist = dist;
        best = neighbor;
        improved = true;
      }
    }
  }
  return best;
}

std::vector<std::pair<float, int>> VectorIndex::search_layer(const float* query, int entry,
                                                             size_t ef, int level) const {
  if (visit_stamp_.size() < nodes_.size()) visit_stamp_.resize(nodes_.size(), 0);
  if (++visit_epoch_ == 0) {
    std::fill(visit_stamp_.begin(), visit_stamp_.end(), 0);
    visit_epoch_ = 1;
  }

  using DistNode = std::pair<float, int>;
  std::priority_queue<DistNode, std::vector<DistNode>, std::greater<DistNode>> frontier;
  std::priority_queue<DistNode> best;

  const float entry_dist =
      l2_distance(query, nodes_[entry].vec.data(), static_cast<size_t>(dim_));
  frontier.emplace(entry_dist, entry);
  best.emplace(entry_dist, entry);
  visit_stamp_[static_cast<size_t>(entry)] = visit_epoch_;

  while (!frontier.empty()) {
    const auto [dist, node] = frontier.top();
    if (dist > best.top().first && best.size() >= ef) break;
    frontier.pop();
    if (level >= static_cast<int>(nodes_[node].links.size())) continue;
    for (int neighbor : nodes_[node].links[static_cast<size_t>(level)]) {
      if (visit_stamp_[static_cast<size_t>(neighbor)] == visit_epoch_) continue;
      visit_stamp_[static_cast<size_t>(neighbor)] = visit_epoch_;
      const float ndist =
          l2_distance(query, nodes_[neighbor].vec.data(), static_cast<size_t>(dim_));
      if (best.size() < ef || ndist < best.top().first) {
        frontier.emplace(ndist, neighbor);
        best.emplace(ndist, neighbor);
        if (best.size() > ef) best.pop();
      }
    }
  }

  std::vector<DistNode> out;
  out.reserve(best.size());
  while (!best.empty()) {
    out.push_back(best.top());
    best.pop();
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<int> VectorIndex::select_neighbors(
    const std::vector<std::pair<float, int>>& candidates, size_t limit) const {
  // HNSW's diversity heuristic: keep a candidate only when it is closer to the
  // query than to every already-selected neighbour. On uniform random
  // high-dimensional data this matters a lot -- plain nearest-M produces a
  // clustered graph and recall collapses at larger N.
  std::vector<int> out;
  out.reserve(std::min(limit, candidates.size()));
  for (const auto& [dist_to_query, candidate] : candidates) {
    if (out.size() >= limit) break;
    bool keep = true;
    for (int chosen : out) {
      const float dist_to_chosen = l2_distance(nodes_[candidate].vec.data(),
                                               nodes_[chosen].vec.data(),
                                               static_cast<size_t>(dim_));
      if (dist_to_chosen < dist_to_query) {
        keep = false;
        break;
      }
    }
    if (keep) out.push_back(candidate);
  }
  if (out.size() < limit) {
    for (const auto& [dist, candidate] : candidates) {
      if (out.size() >= limit) break;
      if (std::find(out.begin(), out.end(), candidate) == out.end()) out.push_back(candidate);
    }
  }
  return out;
}

void VectorIndex::connect(int node, int neighbor, int level) {
  if (level >= static_cast<int>(nodes_[node].links.size())) return;
  auto& links = nodes_[node].links[static_cast<size_t>(level)];
  if (std::find(links.begin(), links.end(), neighbor) != links.end()) return;
  links.push_back(neighbor);
  if (links.size() <= static_cast<size_t>(m_)) return;

  // Keep the m_ closest neighbours; ties fall back to node id for determinism.
  const Node& self = nodes_[node];
  std::vector<std::pair<float, int>> scored;
  scored.reserve(links.size());
  for (int candidate : links) {
    scored.emplace_back(
        l2_distance(self.vec.data(), nodes_[candidate].vec.data(), static_cast<size_t>(dim_)),
        candidate);
  }
  std::sort(scored.begin(), scored.end());
  links.clear();
  for (size_t i = 0; i < scored.size() && i < static_cast<size_t>(m_); ++i) {
    links.push_back(scored[i].second);
  }
}

void VectorIndex::add(int64_t pk, const std::vector<float>& vec, std::vector<Value> row) {
  auto existing = current_.find(pk);
  if (existing != current_.end()) {
    nodes_[static_cast<size_t>(existing->second)].deleted = true;
    ++tombstones_;
    current_.erase(existing);
  }

  const int level = random_level();
  Node node;
  node.vec = vec;
  node.row = std::move(row);
  node.pk = pk;
  node.links.resize(static_cast<size_t>(level) + 1);
  const int id = static_cast<int>(nodes_.size());
  nodes_.push_back(std::move(node));
  current_.emplace(pk, id);
  if (visit_stamp_.size() < nodes_.size()) visit_stamp_.resize(nodes_.size(), 0);

  if (entry_ < 0) {
    entry_ = id;
    max_level_ = level;
    return;
  }

  int ep = entry_;
  for (int l = max_level_; l > level; --l) ep = greedy_closest(vec.data(), ep, l);

  for (int l = std::min(level, max_level_); l >= 0; --l) {
    const auto candidates = search_layer(vec.data(), ep, static_cast<size_t>(ef_construction_), l);
    const auto neighbors = select_neighbors(candidates, static_cast<size_t>(m_));
    for (int neighbor : neighbors) {
      connect(id, neighbor, l);
      connect(neighbor, id, l);
    }
    if (!candidates.empty()) ep = candidates.front().second;
  }

  if (level > max_level_) {
    max_level_ = level;
    entry_ = id;
  }
}

void VectorIndex::remove(int64_t pk) {
  auto it = current_.find(pk);
  if (it == current_.end()) return;
  nodes_[static_cast<size_t>(it->second)].deleted = true;
  ++tombstones_;
  current_.erase(it);
}

std::vector<VectorIndex::Hit> VectorIndex::search(const std::vector<float>& query, size_t k,
                                                  size_t ef, const AcceptFn& accept,
                                                  SearchStats* stats) const {
  std::vector<Hit> out;
  if (entry_ < 0 || k == 0) return out;
  if (ef < k) ef = k;

  int ep = entry_;
  for (int level = max_level_; level > 0; --level) ep = greedy_closest(query.data(), ep, level);

  const auto candidates = search_layer(query.data(), ep, ef, 0);
  if (stats != nullptr) {
    stats->visited = candidates.size();
    stats->accepted = 0;
  }
  for (const auto& [dist, id] : candidates) {
    const Node& node = nodes_[static_cast<size_t>(id)];
    if (node.deleted) continue;
    if (accept && !accept(node.row)) continue;
    if (stats != nullptr) ++stats->accepted;
    out.push_back(Hit{node.pk, dist});
    if (out.size() == k) break;
  }
  return out;
}

}  // namespace mvdb
