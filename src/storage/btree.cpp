#include "storage/btree.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

namespace mvdb {

namespace {

constexpr size_t kNodeHeader = 16;
constexpr uint8_t kNodeLeaf = 1;
constexpr uint8_t kNodeInternal = 2;

constexpr size_t kLeafEntry = 16;      // int64 key + RID
constexpr size_t kInternalEntry = 12;  // int64 key + uint32 child
constexpr size_t kLeafCapacity = (Page::kPageSize - kNodeHeader) / kLeafEntry;
constexpr size_t kInternalCapacity = (Page::kPageSize - kNodeHeader - 4) / kInternalEntry;

constexpr char kMetaMagic[8] = {'M', 'V', 'D', 'B', 'T', 'R', 'E', 'E'};
constexpr uint32_t kMetaVersion = 1;
constexpr uint32_t kMetaBuiltFlag = 1;

uint8_t node_type(const char* d) { return static_cast<uint8_t>(d[0]); }
void set_node_type(char* d, uint8_t value) { d[0] = static_cast<char>(value); }

uint16_t node_count(const char* d) {
  uint16_t value = 0;
  std::memcpy(&value, d + 2, sizeof(value));
  return value;
}
void set_node_count(char* d, uint16_t value) { std::memcpy(d + 2, &value, sizeof(value)); }

int64_t read_i64(const char* p) {
  int64_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}
void write_i64(char* p, int64_t value) { std::memcpy(p, &value, sizeof(value)); }

uint32_t read_u32(const char* p) {
  uint32_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}
void write_u32(char* p, uint32_t value) { std::memcpy(p, &value, sizeof(value)); }

RID read_rid(const char* p) {
  RID value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}
void write_rid(char* p, RID value) { std::memcpy(p, &value, sizeof(value)); }

int64_t leaf_key(const char* d, size_t i) { return read_i64(d + kNodeHeader + i * kLeafEntry); }
RID leaf_rid(const char* d, size_t i) {
  return read_rid(d + kNodeHeader + i * kLeafEntry + 8);
}
void set_leaf(char* d, size_t i, int64_t key, RID rid) {
  write_i64(d + kNodeHeader + i * kLeafEntry, key);
  write_rid(d + kNodeHeader + i * kLeafEntry + 8, rid);
}

int64_t internal_key(const char* d, size_t i) {
  return read_i64(d + kNodeHeader + 4 + i * kInternalEntry);
}
uint32_t internal_child(const char* d, size_t i) {
  if (i == 0) return read_u32(d + kNodeHeader);
  return read_u32(d + kNodeHeader + 4 + (i - 1) * kInternalEntry + 8);
}
void set_internal(char* d, size_t i, int64_t key, uint32_t right_child) {
  write_i64(d + kNodeHeader + 4 + i * kInternalEntry, key);
  write_u32(d + kNodeHeader + 4 + i * kInternalEntry + 8, right_child);
}
void set_first_child(char* d, uint32_t child) { write_u32(d + kNodeHeader, child); }

}  // namespace

BTree::BTree(std::string path, size_t pool_pages) : path_(std::move(path)), pool_pages_(pool_pages) {}

BTree::~BTree() { close(); }

uint32_t BTree::alloc_page() {
  const uint32_t id = page_count_++;
  auto page = pool_->fetch(id);
  page->zero();
  page->set_page_id(id);
  pool_->mark_dirty(page);
  return id;
}

void BTree::init_empty_leaf(uint32_t page_id) {
  auto page = pool_->fetch(page_id);
  page->zero();
  page->set_page_id(page_id);
  set_node_type(page->raw(), kNodeLeaf);
  set_node_count(page->raw(), 0);
  pool_->mark_dirty(page);
}

// The meta page deliberately leaves bytes [8, 16) to the page LSN written by
// the commit path; everything else lives at 16 and above.
void BTree::load_meta() {
  auto page = pool_->fetch(0);
  const char* d = page->raw();
  const uint32_t version = read_u32(d + 16);
  if (version != kMetaVersion) {
    root_ = 0;
    height_ = 0;
    built_ = false;
    count_ = 0;
    return;
  }
  root_ = read_u32(d + 20);
  height_ = read_u32(d + 24);
  const uint32_t flags = read_u32(d + 28);
  built_ = (flags & kMetaBuiltFlag) != 0;
  uint64_t count = 0;
  std::memcpy(&count, d + 32, sizeof(count));
  count_ = count;
}

void BTree::store_meta() {
  auto page = pool_->fetch(0);
  char* d = page->raw();
  page->zero();
  std::memcpy(d, kMetaMagic, sizeof(kMetaMagic));
  write_u32(d + 16, kMetaVersion);
  write_u32(d + 20, root_);
  write_u32(d + 24, height_);
  write_u32(d + 28, built_ ? kMetaBuiltFlag : 0);
  std::memcpy(d + 32, &count_, sizeof(count_));
  pool_->mark_dirty(page);
}

bool BTree::open() {
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) return false;
  struct stat st {};
  if (::fstat(fd_, &st) != 0) return false;
  page_count_ = static_cast<uint32_t>(st.st_size / static_cast<off_t>(Page::kPageSize));
  pool_ = std::make_unique<BufferPool>(fd_, pool_pages_);
  if (page_count_ == 0) {
    alloc_page();  // meta page 0
    root_ = alloc_page();
    init_empty_leaf(root_);
    height_ = 1;
    count_ = 0;
    built_ = false;
    store_meta();
  } else {
    load_meta();
    if (root_ == 0 || height_ == 0) {
      // Unreadable or pre-LSN meta page: start the index over. `built_` stays
      // false so the engine repopulates it from the heap.
      if (::ftruncate(fd_, 0) != 0) return false;
      ::lseek(fd_, 0, SEEK_SET);
      page_count_ = 0;
      pool_->clear();
      alloc_page();
      root_ = alloc_page();
      init_empty_leaf(root_);
      height_ = 1;
      count_ = 0;
      built_ = false;
      store_meta();
    }
  }
  return true;
}

void BTree::close() {
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

bool BTree::mark_built() {
  if (!pool_) return false;
  built_ = true;
  store_meta();
  return pool_->sync();
}

bool BTree::sync() { return pool_ ? pool_->sync() : false; }

void BTree::set_current_txn(uint64_t txid) {
  if (pool_) pool_->set_current_txn(txid);
}

void BTree::release_txn(uint64_t txid) {
  if (pool_) pool_->release_txn(txid);
}

std::vector<BufferPool::DirtyPage> BTree::collect_dirty(uint64_t txid) {
  if (!pool_) return {};
  return pool_->collect_dirty(txid);
}

bool BTree::search(int64_t key, RID& rid) {
  uint32_t page_id = root_;
  for (int depth = 0; depth < 64; ++depth) {
    auto page = pool_->fetch(page_id);
    const char* d = page->raw();
    const uint8_t type = node_type(d);
    const size_t n = node_count(d);
    if (type == kNodeLeaf) {
      size_t lo = 0, hi = n;
      while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (leaf_key(d, mid) < key) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      if (lo < n && leaf_key(d, lo) == key) {
        rid = leaf_rid(d, lo);
        return true;
      }
      return false;
    }
    size_t lo = 0, hi = n;
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (internal_key(d, mid) <= key) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    page_id = internal_child(d, lo);
  }
  return false;
}

bool BTree::insert_rec(uint32_t page_id, int64_t key, RID rid, Split& out, bool& replaced) {
  auto page = pool_->fetch(page_id);
  char* d = page->raw();
  const uint8_t type = node_type(d);
  const size_t n = node_count(d);

  if (type == kNodeLeaf) {
    std::vector<std::pair<int64_t, RID>> entries;
    entries.reserve(n + 1);
    for (size_t i = 0; i < n; ++i) entries.emplace_back(leaf_key(d, i), leaf_rid(d, i));
    auto it = std::lower_bound(entries.begin(), entries.end(), key,
                               [](const auto& entry, int64_t value) { return entry.first < value; });
    if (it != entries.end() && it->first == key) {
      it->second = rid;
      replaced = true;
    } else {
      entries.insert(it, {key, rid});
    }
    if (entries.size() <= kLeafCapacity) {
      page->zero();
      page->set_page_id(page_id);
      set_node_type(d, kNodeLeaf);
      set_node_count(d, static_cast<uint16_t>(entries.size()));
      for (size_t i = 0; i < entries.size(); ++i) set_leaf(d, i, entries[i].first, entries[i].second);
      pool_->mark_dirty(page);
      return true;
    }
    const size_t mid = entries.size() / 2;
    const uint32_t right_id = alloc_page();
    auto right = pool_->fetch(right_id);
    right->zero();
    right->set_page_id(right_id);
    set_node_type(right->raw(), kNodeLeaf);
    set_node_count(right->raw(), static_cast<uint16_t>(entries.size() - mid));
    for (size_t i = mid; i < entries.size(); ++i) {
      set_leaf(right->raw(), i - mid, entries[i].first, entries[i].second);
    }
    page->zero();
    page->set_page_id(page_id);
    set_node_type(d, kNodeLeaf);
    set_node_count(d, static_cast<uint16_t>(mid));
    for (size_t i = 0; i < mid; ++i) set_leaf(d, i, entries[i].first, entries[i].second);
    pool_->mark_dirty(page);
    pool_->mark_dirty(right);
    out.split = true;
    out.key = entries[mid].first;
    out.right = right_id;
    return true;
  }

  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (internal_key(d, mid) <= key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  const uint32_t child_id = internal_child(d, lo);
  Split child_split;
  if (!insert_rec(child_id, key, rid, child_split, replaced)) return false;
  if (!child_split.split) return true;

  std::vector<int64_t> keys;
  std::vector<uint32_t> children;
  keys.reserve(n + 1);
  children.reserve(n + 2);
  for (size_t i = 0; i < n; ++i) {
    keys.push_back(internal_key(d, i));
    children.push_back(internal_child(d, i));
  }
  children.push_back(internal_child(d, n));
  keys.insert(keys.begin() + static_cast<long>(lo), child_split.key);
  children.insert(children.begin() + static_cast<long>(lo) + 1, child_split.right);

  page->zero();
  page->set_page_id(page_id);
  if (keys.size() <= kInternalCapacity) {
    set_node_type(d, kNodeInternal);
    set_node_count(d, static_cast<uint16_t>(keys.size()));
    set_first_child(d, children[0]);
    for (size_t i = 0; i < keys.size(); ++i) set_internal(d, i, keys[i], children[i + 1]);
    pool_->mark_dirty(page);
    return true;
  }

  const size_t mid = keys.size() / 2;
  const uint32_t right_id = alloc_page();
  auto right = pool_->fetch(right_id);
  right->zero();
  right->set_page_id(right_id);
  set_node_type(right->raw(), kNodeInternal);
  set_node_count(right->raw(), static_cast<uint16_t>(keys.size() - mid - 1));
  set_first_child(right->raw(), children[mid + 1]);
  for (size_t i = mid + 1; i < keys.size(); ++i) {
    set_internal(right->raw(), i - mid - 1, keys[i], children[i + 1]);
  }
  set_node_type(d, kNodeInternal);
  set_node_count(d, static_cast<uint16_t>(mid));
  set_first_child(d, children[0]);
  for (size_t i = 0; i < mid; ++i) set_internal(d, i, keys[i], children[i + 1]);
  pool_->mark_dirty(page);
  pool_->mark_dirty(right);
  out.split = true;
  out.key = keys[mid];
  out.right = right_id;
  return true;
}

bool BTree::insert(int64_t key, RID rid) {
  if (!pool_) return false;
  Split split;
  bool replaced = false;
  if (!insert_rec(root_, key, rid, split, replaced)) return false;
  if (split.split) {
    const uint32_t new_root = alloc_page();
    auto page = pool_->fetch(new_root);
    page->zero();
    page->set_page_id(new_root);
    set_node_type(page->raw(), kNodeInternal);
    set_node_count(page->raw(), 1);
    set_first_child(page->raw(), root_);
    set_internal(page->raw(), 0, split.key, split.right);
    pool_->mark_dirty(page);
    root_ = new_root;
    ++height_;
  }
  if (!replaced) ++count_;
  store_meta();
  return true;
}

bool BTree::erase(int64_t key) {
  if (!pool_) return false;
  uint32_t page_id = root_;
  for (int depth = 0; depth < 64; ++depth) {
    auto page = pool_->fetch(page_id);
    char* d = page->raw();
    const uint8_t type = node_type(d);
    const size_t n = node_count(d);
    if (type == kNodeLeaf) {
      size_t lo = 0, hi = n;
      while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (leaf_key(d, mid) < key) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      if (lo >= n || leaf_key(d, lo) != key) return false;
      for (size_t i = lo; i + 1 < n; ++i) {
        set_leaf(d, i, leaf_key(d, i + 1), leaf_rid(d, i + 1));
      }
      set_node_count(d, static_cast<uint16_t>(n - 1));
      pool_->mark_dirty(page);
      if (count_ > 0) --count_;
      store_meta();
      return true;
    }
    size_t lo = 0, hi = n;
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (internal_key(d, mid) <= key) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    page_id = internal_child(d, lo);
  }
  return false;
}

}  // namespace mvdb
