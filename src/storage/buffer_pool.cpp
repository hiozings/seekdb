#include "storage/buffer_pool.h"

#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace mvdb {

namespace {

constexpr off_t page_offset(uint32_t page_id) {
  return static_cast<off_t>(page_id) * static_cast<off_t>(Page::kPageSize);
}

}  // namespace

BufferPool::BufferPool(int fd, size_t capacity_pages, std::string data_path)
    : fd_(fd), data_path_(std::move(data_path)),
      capacity_(capacity_pages < 4 ? 4 : capacity_pages) {}

BufferPool::~BufferPool() { close_spill(); }

void BufferPool::close_spill() noexcept {
  if (spill_fd_ >= 0) ::close(spill_fd_);
  spill_fd_ = -1;
}

void BufferPool::spill(uint32_t page_id, const Page& page) {
  if (spill_fd_ < 0) {
    auto directory = std::filesystem::path(data_path_).parent_path();
    if (directory.empty()) directory = ".";
    std::string pattern = (directory / ".mvdb-spill-XXXXXX").string();
    const int fd = ::mkostemp(pattern.data(), O_CLOEXEC);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "create transaction spill");
    if (::unlink(pattern.c_str()) != 0) {
      const int error = errno;
      ::close(fd);
      throw std::system_error(error, std::generic_category(), "unlink transaction spill");
    }
    spill_fd_ = fd;
  }
  if (!write_page(spill_fd_, page_id, page)) {
    throw std::system_error(errno, std::generic_category(), "write transaction spill");
  }
  spilled_[page_id] = SpilledPage{page.owner_txn(), page.lsn()};
}

bool BufferPool::read_page(int fd, uint32_t page_id, Page& page) {
  size_t bytes = 0;
  while (bytes < Page::kPageSize) {
    const ssize_t n = ::pread(fd, page.raw() + bytes, Page::kPageSize - bytes,
                              page_offset(page_id) + static_cast<off_t>(bytes));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) break;
    bytes += static_cast<size_t>(n);
  }
  if (bytes == 0) {
    page.reset();
  } else if (bytes < Page::kPageSize) {
    std::memset(page.raw() + bytes, 0, Page::kPageSize - bytes);
  }
  page.set_page_id(page_id);
  page.set_dirty(false);
  return true;
}

bool BufferPool::write_page(int fd, uint32_t page_id, const Page& page) {
  size_t written = 0;
  while (written < Page::kPageSize) {
    const ssize_t n = ::pwrite(fd, page.raw() + written, Page::kPageSize - written,
                               page_offset(page_id) + static_cast<off_t>(written));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

bool BufferPool::evict_one() {
  // A page whose use_count is above one is still held by a caller; evicting it
  // would let a later fetch build a second, stale copy of the same page.
  Frame* victim = nullptr;
  uint32_t victim_id = 0;
  for (auto& entry : frames_) {
    if (entry.second.page.use_count() > 1) continue;
    if (victim == nullptr || entry.second.tick < victim->tick) {
      victim = &entry.second;
      victim_id = entry.first;
    }
  }
  if (victim == nullptr) return false;
  if (victim->page->dirty()) {
    if (victim->page->owner_txn() != 0) {
      spill(victim_id, *victim->page);
    } else if (!write_page(fd_, victim_id, *victim->page)) {
      throw std::system_error(errno, std::generic_category(), "write evicted page");
    }
  }
  frames_.erase(victim_id);
  return true;
}

std::shared_ptr<Page> BufferPool::fetch(uint32_t page_id) {
  auto it = frames_.find(page_id);
  if (it != frames_.end()) {
    it->second.tick = ++tick_;
    return it->second.page;
  }
  while (frames_.size() >= capacity_ && evict_one()) {}

  auto page = std::make_shared<Page>();
  const auto spilled = spilled_.find(page_id);
  const int source = spilled == spilled_.end() ? fd_ : spill_fd_;
  if (!read_page(source, page_id, *page)) {
    throw std::system_error(errno, std::generic_category(), "read cached page");
  }
  if (spilled != spilled_.end()) {
    page->set_owner_txn(spilled->second.owner_txn);
    page->set_lsn(spilled->second.lsn);
    page->set_dirty(true);
  }
  frames_.emplace(page_id, Frame{page, ++tick_});
  if (spilled != spilled_.end()) spilled_.erase(spilled);
  return page;
}

void BufferPool::mark_dirty(const std::shared_ptr<Page>& page) {
  if (!page) return;
  page->set_dirty(true);
  if (current_txn_ != 0) page->set_owner_txn(current_txn_);
}

std::vector<BufferPool::DirtyPage> BufferPool::collect_dirty(uint64_t txid) {
  // Compatibility API: returned handles pin all selected pages. Large commits
  // use visit_dirty instead to keep their working set bounded.
  std::vector<uint32_t> ids;
  for (const auto& entry : spilled_) {
    if (entry.second.owner_txn == txid) ids.push_back(entry.first);
  }
  for (const auto& entry : frames_) {
    if (entry.second.page->dirty() && entry.second.page->owner_txn() == txid) {
      ids.push_back(entry.first);
    }
  }
  std::vector<DirtyPage> out;
  out.reserve(ids.size());
  for (uint32_t id : ids) out.push_back(DirtyPage{id, fetch(id)});
  return out;
}

std::vector<uint32_t> BufferPool::spilled_page_ids(uint64_t txid) const {
  std::vector<uint32_t> ids;
  ids.reserve(spilled_.size());
  for (const auto& entry : spilled_) {
    if (entry.second.owner_txn == txid && frames_.count(entry.first) == 0) ids.push_back(entry.first);
  }
  // Sequential offsets let the OS read ahead instead of issuing scattered
  // small reads when a transaction is larger than available page cache.
  std::sort(ids.begin(), ids.end());
  return ids;
}

void BufferPool::visit_dirty(uint64_t txid,
                             const std::function<void(uint32_t, Page&)>& visitor) {
  for (uint32_t id : spilled_page_ids(txid)) {
    auto& pending = spilled_.at(id);
    Page page;
    if (!read_page(spill_fd_, id, page)) {
      throw std::system_error(errno, std::generic_category(), "read transaction spill");
    }
    page.set_owner_txn(txid);
    page.set_dirty(true);
    page.set_lsn(pending.lsn);
    std::array<char, Page::kPageSize> before;
    std::memcpy(before.data(), page.raw(), before.size());
    visitor(id, page);
    // Commit normally changes only bytes [8,16), the LSN. Retain that small
    // change in memory instead of rewriting every spilled page before publish.
    // Other visitor changes still retain the original write-back semantics.
    if (std::memcmp(before.data(), page.raw(), 8) != 0 ||
        std::memcmp(before.data() + 16, page.raw() + 16, Page::kPageSize - 16) != 0) {
      if (!write_page(spill_fd_, id, page)) {
        throw std::system_error(errno, std::generic_category(), "update transaction spill");
      }
    }
    pending.lsn = page.lsn();
  }
  for (auto& entry : frames_) {
    if (entry.second.page->dirty() && entry.second.page->owner_txn() == txid) {
      visitor(entry.first, *entry.second.page);
    }
  }
}

void BufferPool::release_txn(uint64_t txid) {
  // WAL must already be durable, making partially completed publication safe
  // to recover. Retain all ownership if a data write fails.
  for (uint32_t id : spilled_page_ids(txid)) {
    const auto& pending = spilled_.at(id);
    Page page;
    if (!read_page(spill_fd_, id, page)) {
      throw std::system_error(errno, std::generic_category(), "read transaction spill for publication");
    }
    page.set_lsn(pending.lsn);
    if (!write_page(fd_, id, page)) {
      throw std::system_error(errno, std::generic_category(), "publish transaction spill");
    }
  }
  for (auto it = spilled_.begin(); it != spilled_.end();) {
    if (it->second.owner_txn == txid) it = spilled_.erase(it);
    else ++it;
  }
  for (auto& entry : frames_) {
    if (entry.second.page->owner_txn() == txid) entry.second.page->set_owner_txn(0);
  }
  if (current_txn_ == txid) current_txn_ = 0;
  if (spilled_.empty()) close_spill();
}

void BufferPool::flush_all() {
  for (auto& entry : frames_) {
    if (entry.second.page->dirty() && entry.second.page->owner_txn() == 0) {
      if (!write_page(fd_, entry.first, *entry.second.page)) {
        throw std::system_error(errno, std::generic_category(), "flush cached page");
      }
      entry.second.page->set_dirty(false);
    }
  }
}

bool BufferPool::sync() {
  try {
    flush_all();
  } catch (const std::system_error&) {
    return false;
  }
  return ::fdatasync(fd_) == 0;
}

void BufferPool::clear() {
  frames_.clear();
  spilled_.clear();
  close_spill();
  current_txn_ = 0;
}

}  // namespace mvdb
