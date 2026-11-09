#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace mvdb {

// On-disk B+tree mapping BIGINT primary keys to row identifiers.
//
// Page 0 is the meta page; every other page is a leaf or internal node. Leaves
// store (key, rid) pairs and are chained through `next` (reserved for range
// scans). Internal nodes store `count` separator keys and `count + 1` children.
//
// Deletes remove the key from its leaf but do not merge nodes; lookups stay
// correct because routing only needs `rightmost child with separator <= key`.
class BTree {
 public:
  BTree(std::string path, size_t pool_pages);
  ~BTree();

  BTree(const BTree&) = delete;
  BTree& operator=(const BTree&) = delete;

  bool open();
  void close();

  bool search(int64_t key, RID& rid);
  // Inserts the key, or replaces its RID when it already exists.
  bool insert(int64_t key, RID rid);
  bool erase(int64_t key);

  uint64_t size() const { return count_; }
  // False until a full index build has been confirmed on disk. The engine
  // rebuilds from the heap when this is false, which makes recovery from an
  // interrupted build idempotent.
  bool built() const { return built_; }
  bool mark_built();

  bool sync();

 private:
  struct Split {
    bool split = false;
    int64_t key = 0;
    uint32_t right = 0;
  };

  bool insert_rec(uint32_t page_id, int64_t key, RID rid, Split& out, bool& replaced);
  uint32_t alloc_page();
  void init_empty_leaf(uint32_t page_id);
  void load_meta();
  void store_meta();

  std::string path_;
  size_t pool_pages_;
  int fd_ = -1;
  uint32_t page_count_ = 0;
  uint32_t root_ = 0;
  uint32_t height_ = 1;
  uint64_t count_ = 0;
  bool built_ = false;
  std::unique_ptr<BufferPool> pool_;
};

}  // namespace mvdb
