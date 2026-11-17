#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>

#include "storage/page.h"

namespace mvdb {

// A bounded page cache over one file. Pages are returned as shared_ptr so that
// an eviction triggered by a later fetch cannot invalidate a page a caller is
// still holding.
class BufferPool {
 public:
  BufferPool(int fd, size_t capacity_pages, std::string data_path = {});
  ~BufferPool();
  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;
  BufferPool(BufferPool&&) = delete;
  BufferPool& operator=(BufferPool&&) = delete;

  std::shared_ptr<Page> fetch(uint32_t page_id);
  void mark_dirty(const std::shared_ptr<Page>& page);

  // Pages dirtied by `txid` and not yet logged. Their owner is stamped by
  // mark_dirty(). Owned pages may spill to an unlinked temporary file, but
  // never to the data file before the transaction WAL is durable.
  struct DirtyPage {
    uint32_t page_id = 0;
    std::shared_ptr<Page> page;
  };
  std::vector<DirtyPage> collect_dirty(uint64_t txid);
  // Visits one latest image at a time, without materializing the entire txn.
  // The visitor must not re-enter the pool. Changes (e.g. LSN) are retained.
  void visit_dirty(uint64_t txid, const std::function<void(uint32_t, Page&)>& visitor);
  void set_current_txn(uint64_t txid) { current_txn_ = txid; }
  // Call only after WAL is durable. Publishes spilled images before clearing
  // ownership. Throws on I/O failure and retains pending state for recovery.
  void release_txn(uint64_t txid);
  // Writes logged dirty pages only; owned pages remain private.
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

  std::vector<uint32_t> spilled_page_ids(uint64_t txid) const;
  void spill(uint32_t page_id, const Page& page);
  void close_spill() noexcept;

  int fd_;
  std::string data_path_;
  int spill_fd_ = -1;
  struct SpilledPage {
    uint64_t owner_txn = 0;
    uint64_t lsn = 0;  // may be newer than the private on-disk image header
  };
  std::unordered_map<uint32_t, SpilledPage> spilled_;
  size_t capacity_;
  uint64_t tick_ = 0;
  uint64_t current_txn_ = 0;
  std::unordered_map<uint32_t, Frame> frames_;
};

}  // namespace mvdb
