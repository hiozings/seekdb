#include "storage/page.h"

#include <cstring>
#include <vector>

namespace mvdb {

Page::Page() { reset(); }

void Page::reset() {
  std::memset(data_, 0, kPageSize);
  wr16(0, 0);                        // slot_count
  wr16(2, static_cast<uint16_t>(kHeaderSize));  // free_start
  wr16(4, static_cast<uint16_t>(kPageSize));    // free_end
  wr16(6, 0);                        // flags
  set_lsn(0);
  dirty_ = false;
  owner_txn_ = 0;
}

void Page::zero() {
  std::memset(data_, 0, kPageSize);
  dirty_ = false;
  owner_txn_ = 0;
}

uint64_t Page::lsn() const {
  uint64_t value = 0;
  std::memcpy(&value, data_ + 8, sizeof(value));
  return value;
}

void Page::set_lsn(uint64_t value) { std::memcpy(data_ + 8, &value, sizeof(value)); }

uint16_t Page::rd16(size_t off) const {
  uint16_t value = 0;
  std::memcpy(&value, data_ + off, sizeof(value));
  return value;
}

void Page::wr16(size_t off, uint16_t value) { std::memcpy(data_ + off, &value, sizeof(value)); }

uint32_t Page::rd32(size_t off) const {
  uint32_t value = 0;
  std::memcpy(&value, data_ + off, sizeof(value));
  return value;
}

void Page::wr32(size_t off, uint32_t value) { std::memcpy(data_ + off, &value, sizeof(value)); }

uint32_t Page::slot_count() const { return rd16(0); }

bool Page::live(uint16_t slot) const {
  if (slot >= slot_count()) return false;
  return rd16(kHeaderSize + static_cast<size_t>(slot) * 4 + 2) != 0;
}

bool Page::has_dead_slot(size_t& slot) const {
  for (uint16_t i = 0; i < slot_count(); ++i) {
    if (!live(i)) {
      slot = i;
      return true;
    }
  }
  return false;
}

std::string_view Page::get(uint16_t slot) const {
  if (!live(slot)) return {};
  const size_t base = kHeaderSize + static_cast<size_t>(slot) * 4;
  const uint16_t offset = rd16(base);
  const uint16_t len = rd16(base + 2);
  return std::string_view(data_ + offset, len);
}

bool Page::insert(const std::string& record, uint16_t& slot_out) {
  const size_t len = record.size();
  size_t reuse = 0;
  const bool has_reuse = has_dead_slot(reuse);
  const size_t slot_overhead = has_reuse ? 0 : 4;
  const size_t dir_size = kHeaderSize + static_cast<size_t>(slot_count()) * 4;
  if (static_cast<size_t>(free_end()) < dir_size + slot_overhead + len) {
    compact();
  }
  const size_t start = free_start() + slot_overhead;
  if (free_end() < start + len) return false;

  const size_t offset = free_end() - len;
  std::memcpy(data_ + offset, record.data(), len);
  set_free_end(offset);

  uint16_t slot = 0;
  if (has_reuse) {
    slot = static_cast<uint16_t>(reuse);
  } else {
    slot = static_cast<uint16_t>(slot_count());
    wr16(0, static_cast<uint16_t>(slot + 1));
  }
  const size_t base = kHeaderSize + static_cast<size_t>(slot) * 4;
  wr16(base, static_cast<uint16_t>(offset));
  wr16(base + 2, static_cast<uint16_t>(len));
  slot_out = slot;
  return true;
}

bool Page::erase(uint16_t slot) {
  if (!live(slot)) return false;
  const size_t base = kHeaderSize + static_cast<size_t>(slot) * 4;
  wr16(base, 0);
  wr16(base + 2, 0);
  return true;
}

bool Page::rewrite(uint16_t slot, const std::string& record) {
  if (!live(slot)) return false;
  const size_t base = kHeaderSize + static_cast<size_t>(slot) * 4;
  if (record.size() > rd16(base + 2)) return false;
  const size_t offset = rd16(base);
  std::memcpy(data_ + offset, record.data(), record.size());
  wr16(base + 2, static_cast<uint16_t>(record.size()));
  return true;
}

bool Page::can_fit(size_t len) const {
  size_t dead = 0;
  const bool has_reuse = has_dead_slot(dead);
  const size_t slot_overhead = has_reuse ? 0 : 4;
  const size_t dir_size = kHeaderSize + static_cast<size_t>(slot_count()) * 4;
  return static_cast<size_t>(kPageSize) >= dir_size + slot_overhead + len;
}

void Page::compact() {
  std::vector<std::string> records;
  std::vector<uint16_t> slots;
  for (uint16_t i = 0; i < slot_count(); ++i) {
    if (!live(i)) continue;
    std::string_view view = get(i);
    records.emplace_back(view.data(), view.size());
    slots.push_back(i);
  }
  size_t end = kPageSize;
  for (size_t k = 0; k < records.size(); ++k) {
    end -= records[k].size();
    std::memcpy(data_ + end, records[k].data(), records[k].size());
    const size_t base = kHeaderSize + static_cast<size_t>(slots[k]) * 4;
    wr16(base, static_cast<uint16_t>(end));
    wr16(base + 2, static_cast<uint16_t>(records[k].size()));
  }
  set_free_end(end);
  // Dead slots were already zeroed by erase(); compaction keeps their indices.
}

}  // namespace mvdb
