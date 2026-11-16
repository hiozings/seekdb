#pragma once

#include "storage/record_spool.h"
#include "storage/page.h"

namespace mvdb {

class UndoLog {
 public:
  struct Entry {
    enum class Kind { InsertedRow, DeletedRow, UpdatedRow, CreatedTable, CreatedVectorIndex } kind;
    uint32_t table_id = 0;
    int64_t pk = 0;
    RID rid = 0;
    std::string record;
  };
  explicit UndoLog(std::string directory) : records_(std::move(directory)) {}
  size_t size() const { return records_.size(); }
  void clear() { records_.clear(); }
  void push_back(const Entry& entry) {
    std::string bytes;
    bytes.push_back(static_cast<char>(entry.kind));
    bytes.append(reinterpret_cast<const char*>(&entry.table_id), sizeof(entry.table_id));
    bytes.append(reinterpret_cast<const char*>(&entry.pk), sizeof(entry.pk));
    bytes.append(reinterpret_cast<const char*>(&entry.rid), sizeof(entry.rid));
    bytes.append(entry.record);
    records_.append(bytes);
  }
  Entry back() {
    const std::string bytes = records_.back();
    Entry entry;
    entry.kind = static_cast<Entry::Kind>(bytes[0]);
    size_t pos = 1;
    std::memcpy(&entry.table_id, bytes.data()+pos, sizeof(entry.table_id)); pos += sizeof(entry.table_id);
    std::memcpy(&entry.pk, bytes.data()+pos, sizeof(entry.pk)); pos += sizeof(entry.pk);
    std::memcpy(&entry.rid, bytes.data()+pos, sizeof(entry.rid)); pos += sizeof(entry.rid);
    entry.record = bytes.substr(pos);
    return entry;
  }
  void pop_back() { records_.pop_back(); }
 private:
  RecordSpool records_;
};

}  // namespace mvdb
