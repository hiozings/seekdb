#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "storage/page.h"

namespace mvdb {

// A bounded page cache over one file. Pages are returned as shared_ptr so that
// an eviction triggered by a later fetch cannot invalidate a page a caller is
// still holding.
class BufferPool {
 public:
  BufferPool(int fd, size_t capacity_pages);
  ~BufferPool() = default;

  std::shared_ptr<Page> fetch(uint32_t page_id);
  void mark_dirty(const std::shared_ptr<Page>& page);
  // Writes every dirty page back to the file.
  void flush_all();
  // flush_all() followed by fdatasync.
  bool sync();
  // Drops every cached page (used on close and after errors).
  void clear();
  size_t size() const { return frames_.size(); }

  static bool read_page(int fd, uint32_t page_id, Page& page);
  static bool write_page(int fd, uint32_t page_id, const Page& page);

 private:
  struct Frame {
    std::shared_ptr<Page> page;
    uint64_t tick = 0;
  };

  // Returns false when every cached page is pinned by a caller.
  bool evict_one();

  int fd_;
  size_t capacity_;
  uint64_t tick_ = 0;
  std::unordered_map<uint32_t, Frame> frames_;
};

}  // namespace mvdb
