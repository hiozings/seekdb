#include "storage/buffer_pool.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mvdb {

namespace {

constexpr off_t page_offset(uint32_t page_id) {
  return static_cast<off_t>(page_id) * static_cast<off_t>(Page::kPageSize);
}

}  // namespace

BufferPool::BufferPool(int fd, size_t capacity_pages)
    : fd_(fd), capacity_(capacity_pages < 4 ? 4 : capacity_pages) {}

bool BufferPool::read_page(int fd, uint32_t page_id, Page& page) {
  const ssize_t n = ::pread(fd, page.raw(), Page::kPageSize, page_offset(page_id));
  if (n < 0) return false;
  if (n == 0) {
    page.reset();
  } else if (static_cast<size_t>(n) < Page::kPageSize) {
    // Partially written page at the tail: treat the remainder as empty rather
    // than propagating garbage into the slot directory.
    std::memset(page.raw() + n, 0, Page::kPageSize - static_cast<size_t>(n));
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

void BufferPool::evict_one() {
  if (frames_.empty()) return;
  auto victim = frames_.begin();
  for (auto it = frames_.begin(); it != frames_.end(); ++it) {
    if (it->second.tick < victim->second.tick) victim = it;
  }
  if (victim->second.page->dirty()) {
    write_page(fd_, victim->first, *victim->second.page);
  }
  frames_.erase(victim);
}

std::shared_ptr<Page> BufferPool::fetch(uint32_t page_id) {
  auto it = frames_.find(page_id);
  if (it != frames_.end()) {
    it->second.tick = ++tick_;
    return it->second.page;
  }
  if (frames_.size() >= capacity_) evict_one();

  auto page = std::make_shared<Page>();
  read_page(fd_, page_id, *page);
  frames_.emplace(page_id, Frame{page, ++tick_});
  return page;
}

void BufferPool::mark_dirty(const std::shared_ptr<Page>& page) {
  if (page) page->set_dirty(true);
}

void BufferPool::flush_all() {
  for (auto& entry : frames_) {
    if (entry.second.page->dirty()) {
      write_page(fd_, entry.first, *entry.second.page);
      entry.second.page->set_dirty(false);
    }
  }
}

bool BufferPool::sync() {
  flush_all();
  return ::fdatasync(fd_) == 0;
}

void BufferPool::clear() { frames_.clear(); }

}  // namespace mvdb
