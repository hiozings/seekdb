#include "storage/heap_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mvdb {

HeapFile::HeapFile(std::string path, size_t pool_pages)
    : path_(std::move(path)), pool_pages_(pool_pages) {}

HeapFile::~HeapFile() { close(); }

bool HeapFile::open() {
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) return false;
  struct stat st {};
  if (::fstat(fd_, &st) != 0) return false;
  page_count_ = static_cast<uint32_t>(st.st_size / static_cast<off_t>(Page::kPageSize));
  pool_ = std::make_unique<BufferPool>(fd_, pool_pages_);
  return true;
}

void HeapFile::close() {
  if (pool_) {
    pool_->sync();
    pool_->clear();
    pool_.reset();
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

RID HeapFile::insert_into(uint32_t page_id, const std::string& record) {
  auto page = pool_->fetch(page_id);
  if (!page->can_fit(record.size())) return kInvalidRid;
  uint16_t slot = 0;
  if (!page->insert(record, slot)) return kInvalidRid;
  pool_->mark_dirty(page);
  return make_rid(page_id, slot);
}

RID HeapFile::insert(const std::string& record) {
  if (record.size() + Page::kHeaderSize + 4 > Page::kPageSize) return kInvalidRid;
  for (uint32_t p = hint_page_; p < page_count_; ++p) {
    const RID rid = insert_into(p, record);
    if (rid != kInvalidRid) {
      hint_page_ = p;
      return rid;
    }
  }
  // Reuse space freed by deletes, probing a bounded number of candidates.
  size_t probed = 0;
  for (auto it = spare_pages_.begin(); it != spare_pages_.end() && probed < 8;) {
    const uint32_t p = *it;
    ++probed;
    const RID rid = insert_into(p, record);
    if (rid != kInvalidRid) {
      hint_page_ = p;
      return rid;
    }
    it = spare_pages_.erase(it);
  }
  const uint32_t new_page = page_count_++;
  auto page = pool_->fetch(new_page);
  page->reset();
  page->set_page_id(new_page);
  uint16_t slot = 0;
  if (!page->insert(record, slot)) return kInvalidRid;
  pool_->mark_dirty(page);
  hint_page_ = new_page;
  return make_rid(new_page, slot);
}

bool HeapFile::read(RID rid, std::string& out) {
  const uint32_t page_id = rid_page(rid);
  if (page_id >= page_count_) return false;
  auto page = pool_->fetch(page_id);
  const uint16_t slot = rid_slot(rid);
  if (!page->live(slot)) return false;
  std::string_view view = page->get(slot);
  out.assign(view.data(), view.size());
  return true;
}

bool HeapFile::erase(RID rid) {
  const uint32_t page_id = rid_page(rid);
  if (page_id >= page_count_) return false;
  auto page = pool_->fetch(page_id);
  if (!page->erase(rid_slot(rid))) return false;
  pool_->mark_dirty(page);
  spare_pages_.insert(page_id);
  return true;
}

RID HeapFile::update(RID rid, const std::string& record) {
  const uint32_t page_id = rid_page(rid);
  if (page_id >= page_count_) return kInvalidRid;
  auto page = pool_->fetch(page_id);
  if (page->rewrite(rid_slot(rid), record)) {
    pool_->mark_dirty(page);
    return rid;
  }
  page->erase(rid_slot(rid));
  pool_->mark_dirty(page);
  return insert(record);
}

void HeapFile::scan(const std::function<void(RID, std::string_view)>& fn) {
  for (uint32_t p = 0; p < page_count_; ++p) {
    auto page = pool_->fetch(p);
    const uint32_t slots = page->slot_count();
    for (uint32_t s = 0; s < slots; ++s) {
      const uint16_t slot = static_cast<uint16_t>(s);
      if (!page->live(slot)) continue;
      fn(make_rid(p, slot), page->get(slot));
    }
  }
}

bool HeapFile::sync() {
  if (!pool_) return false;
  return pool_->sync();
}

void HeapFile::set_current_txn(uint64_t txid) {
  if (pool_) pool_->set_current_txn(txid);
}

void HeapFile::release_txn(uint64_t txid) {
  if (pool_) pool_->release_txn(txid);
}

std::vector<BufferPool::DirtyPage> HeapFile::collect_dirty(uint64_t txid) {
  if (!pool_) return {};
  return pool_->collect_dirty(txid);
}

}  // namespace mvdb
