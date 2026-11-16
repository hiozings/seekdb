#include "storage/heap_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
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
  if (::fstat(fd_, &st) != 0) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  page_count_ = static_cast<uint32_t>(st.st_size / static_cast<off_t>(Page::kPageSize));
  free_overflow_pages_.clear();
  // Recovery has already restored committed images. Reconstruct this derived
  // allocation state from headers without loading every page into the cache.
  for (uint32_t id = 0; id < page_count_; ++id) {
    char header[Page::kHeaderSize];
    size_t read = 0;
    while (read < sizeof(header)) {
      const ssize_t count = ::pread(fd_, header + read, sizeof(header) - read,
          static_cast<off_t>(id) * Page::kPageSize + read);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) {
        ::close(fd_);
        fd_ = -1;
        free_overflow_pages_.clear();
        return false;
      }
      read += static_cast<size_t>(count);
    }
    uint16_t flags = 0;
    uint32_t chunk_length = 0;
    std::memcpy(&flags, header + 6, sizeof(flags));
    std::memcpy(&chunk_length, header + 20, sizeof(chunk_length));
    if ((flags & 1u) != 0 && chunk_length == 0) free_overflow_pages_.insert(id);
  }
  pool_ = std::make_unique<BufferPool>(fd_, pool_pages_, path_);
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
  if (record.size() > Page::kPageSize - Page::kHeaderSize - 4) return insert_overflow(record);
  for (uint32_t p = hint_page_; p < page_count_ && p - hint_page_ < 8; ++p) {
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

RID HeapFile::insert_overflow(const std::string& record) {
  constexpr uint32_t end = std::numeric_limits<uint32_t>::max();
  uint32_t first = end;
  size_t remaining = record.size();
  while (remaining != 0) {
    const size_t size = std::min(remaining, Page::kPageSize - Page::kHeaderSize);
    const size_t start = remaining - size;
    uint32_t id;
    if (free_overflow_pages_.empty()) {
      id = page_count_++;
    } else {
      auto it = free_overflow_pages_.begin();
      id = *it;
      free_overflow_pages_.erase(it);
    }
    auto page = pool_->fetch(id);
    page->set_overflow_page(first, std::string_view(record).substr(start, size));
    page->set_page_id(id);
    pool_->mark_dirty(page);
    first = id;
    remaining = start;
  }
  std::string pointer(sizeof(first), '\0');
  std::memcpy(pointer.data(), &first, sizeof(first));
  const RID rid = insert(pointer);
  if (rid == kInvalidRid) {
    free_overflow(first);
    return rid;
  }
  auto page = pool_->fetch(rid_page(rid));
  page->set_overflow(rid_slot(rid));
  pool_->mark_dirty(page);
  return rid;
}

bool HeapFile::read_overflow(uint32_t first, std::string& out) {
  constexpr uint32_t end = std::numeric_limits<uint32_t>::max();
  out.clear();
  size_t visited = 0;
  while (first != end) {
    if (first >= page_count_ || ++visited > page_count_) return false;
    auto page = pool_->fetch(first);
    const auto chunk = page->overflow_chunk();
    if (chunk.empty()) return false;
    out.append(chunk.data(), chunk.size());
    first = page->overflow_next();
  }
  return true;
}

void HeapFile::free_overflow(uint32_t first) {
  constexpr uint32_t end = std::numeric_limits<uint32_t>::max();
  size_t visited = 0;
  while (first != end && first < page_count_ && ++visited <= page_count_) {
    auto page = pool_->fetch(first);
    if (!page->is_overflow_page()) break;
    const uint32_t next = page->overflow_next();
    // Keep the reserved page flag: ordinary inserts must never reuse a page
    // that still appears in free_overflow_pages_.
    page->set_overflow_page(end, {});
    pool_->mark_dirty(page);
    free_overflow_pages_.insert(first);
    first = next;
  }
}

bool HeapFile::read(RID rid, std::string& out) {
  const uint32_t page_id = rid_page(rid);
  if (page_id >= page_count_) return false;
  auto page = pool_->fetch(page_id);
  const uint16_t slot = rid_slot(rid);
  if (!page->live(slot)) return false;
  std::string_view view = page->get(slot);
  if (page->is_overflow(slot)) {
    if (view.size() != sizeof(uint32_t)) return false;
    uint32_t first;
    std::memcpy(&first, view.data(), sizeof(first));
    return read_overflow(first, out);
  }
  out.assign(view.data(), view.size());
  return true;
}

bool HeapFile::erase(RID rid) {
  const uint32_t page_id = rid_page(rid);
  if (page_id >= page_count_) return false;
  auto page = pool_->fetch(page_id);
  const uint16_t slot = rid_slot(rid);
  if (page->is_overflow(slot)) {
    const auto pointer = page->get(slot);
    if (pointer.size() != sizeof(uint32_t)) return false;
    uint32_t first;
    std::memcpy(&first, pointer.data(), sizeof(first));
    free_overflow(first);
  }
  if (!page->erase(slot)) return false;
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
  // Preserve the old row if allocation fails.
  // Keep the page pinned while insert may compact it or allocate another page.
  const RID replacement = insert(record);
  if (replacement == kInvalidRid) return kInvalidRid;
  erase(rid);
  return replacement;
}

void HeapFile::scan(const std::function<void(RID, std::string_view)>& fn) {
  for (uint32_t p = 0; p < page_count_; ++p) {
    auto page = pool_->fetch(p);
    if (page->is_overflow_page()) continue;
    const uint32_t slots = page->slot_count();
    for (uint32_t s = 0; s < slots; ++s) {
      const uint16_t slot = static_cast<uint16_t>(s);
      if (!page->live(slot)) continue;
      const RID rid = make_rid(p, slot);
      if (page->is_overflow(slot)) {
        std::string record;
        if (read(rid, record)) fn(rid, record);
      } else {
        fn(rid, page->get(slot));
      }
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

void HeapFile::visit_dirty(uint64_t txid, const std::function<void(uint32_t, Page&)>& visitor) {
  if (pool_) pool_->visit_dirty(txid, visitor);
}

std::vector<BufferPool::DirtyPage> HeapFile::collect_dirty(uint64_t txid) {
  if (!pool_) return {};
  return pool_->collect_dirty(txid);
}

}  // namespace mvdb
