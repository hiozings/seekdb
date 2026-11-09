#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mvdb {

// A row identifier: page number in the high 16 bits, slot number in the low 16.
using RID = uint64_t;

// RID 0 is a legitimate value (page 0, slot 0), so failure needs its own value.
constexpr RID kInvalidRid = ~static_cast<RID>(0);

inline RID make_rid(uint32_t page_id, uint16_t slot) {
  return (static_cast<RID>(page_id) << 16) | slot;
}
inline uint32_t rid_page(RID rid) { return static_cast<uint32_t>(rid >> 16); }
inline uint16_t rid_slot(RID rid) { return static_cast<uint16_t>(rid & 0xFFFFu); }

// Slotted page: a slot directory grows from the front, records from the back.
//
//  off 0  uint16 slot_count
//  off 2  uint16 free_start     first free byte after the slot directory
//  off 4  uint16 free_end       first byte of the record area
//  off 6  uint16 flags
//  off 8  uint32 next_page      reserved for page chains
//  off 12 uint64 lsn            reserved for write-ahead logging
//  off 20 4 bytes reserved
//
// Each slot is 4 bytes: uint16 offset, uint16 length. length == 0 marks a dead
// slot, which is reusable by a later insert.
class Page {
 public:
  static constexpr size_t kPageSize = 8192;
  static constexpr size_t kHeaderSize = 24;

  Page();

  void reset();
  // Zeroes the page without installing the slotted-page header. Used by
  // structures with their own on-page layout (the B+tree).
  void zero();

  // Inserts a record, reusing a dead slot when possible.
  bool insert(const std::string& record, uint16_t& slot_out);
  std::string_view get(uint16_t slot) const;
  bool live(uint16_t slot) const;
  bool erase(uint16_t slot);
  // Rewrites in place only when the new record fits the existing slot.
  bool rewrite(uint16_t slot, const std::string& record);
  // True when a record of `len` bytes can fit after compaction.
  bool can_fit(size_t len) const;
  void compact();

  uint32_t slot_count() const;

  bool dirty() const { return dirty_; }
  void set_dirty(bool value) { dirty_ = value; }
  uint32_t page_id() const { return page_id_; }
  void set_page_id(uint32_t id) { page_id_ = id; }

  char* raw() { return data_; }
  const char* raw() const { return data_; }

 private:
  uint16_t rd16(size_t off) const;
  void wr16(size_t off, uint16_t value);
  uint32_t rd32(size_t off) const;
  void wr32(size_t off, uint32_t value);
  size_t free_start() const { return kHeaderSize + static_cast<size_t>(slot_count()) * 4; }
  size_t free_end() const { return rd16(4); }
  void set_free_end(size_t value) { wr16(4, static_cast<uint16_t>(value)); }
  bool has_dead_slot(size_t& slot) const;

  char data_[kPageSize];
  bool dirty_ = false;
  uint32_t page_id_ = 0;
};

}  // namespace mvdb
