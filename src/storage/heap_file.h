#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace mvdb {

// An append-only heap of slotted pages holding schema-encoded rows.
// Page contents are cached in a bounded pool; nothing is assumed to fit in
// memory, so scans always stream page by page.
class HeapFile {
 public:
  HeapFile(std::string path, size_t pool_pages);
  ~HeapFile();

  HeapFile(const HeapFile&) = delete;
  HeapFile& operator=(const HeapFile&) = delete;

  bool open();
  void close();

  uint32_t page_count() const { return page_count_; }

  // Returns an invalid RID (0xFFFF...) when the record cannot be stored.
  RID insert(const std::string& record);
  // Copies the record bytes into `out`; returns false when the RID is dead.
  bool read(RID rid, std::string& out);
  bool erase(RID rid);
  // Rewrites in place when the new record fits, otherwise erases and reinserts.
  RID update(RID rid, const std::string& record);

  // Streams every live record. The callback must copy anything it retains.
  void scan(const std::function<void(RID, std::string_view)>& fn);

  // Writes dirty pages back and fsyncs.
  bool sync();

 private:
  bool ensure_capacity(uint32_t upto_page);
  RID insert_into(uint32_t page_id, const std::string& record);
  void try_evict();

  std::string path_;
  size_t pool_pages_;
  int fd_ = -1;
  uint32_t page_count_ = 0;
  uint32_t hint_page_ = 0;
  // Pages that may still have room because rows were deleted from them. Bounded
  // probing here replaces an earlier full-file backscan that made inserts O(n^2).
  std::set<uint32_t> spare_pages_;
  std::unique_ptr<BufferPool> pool_;
};

}  // namespace mvdb
