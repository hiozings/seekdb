/**
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "common/ob_tablet_id.h"

namespace oceanbase
{
namespace sql
{

struct ObFtsCompactEntry
{
  uint64_t table_id_;
  common::ObTabletID tablet_id_;
  int64_t schema_version_;
  uint64_t word_col_id_;
  uint64_t docs_col_id_;
  uint64_t doc_count_col_id_;
  int64_t total_doc_cnt_;
  double avg_doc_len_;

  ObFtsCompactEntry()
      : table_id_(common::OB_INVALID_ID), tablet_id_(), schema_version_(-1),
        word_col_id_(common::OB_INVALID_ID), docs_col_id_(common::OB_INVALID_ID),
        doc_count_col_id_(common::OB_INVALID_ID), total_doc_cnt_(0), avg_doc_len_(0.0) {}
  bool is_valid() const
  {
    return common::OB_INVALID_ID != table_id_ && tablet_id_.is_valid()
        && schema_version_ >= 0 && common::OB_INVALID_ID != word_col_id_
        && common::OB_INVALID_ID != docs_col_id_ && common::OB_INVALID_ID != doc_count_col_id_;
  }
};

// Process-wide mapping from the FTS index aux table id to the compact posting
// table metadata. Populated by the FTS DDL parent task after a successful
// build; queries fall back to the row-store path when an entry is absent
// (for example after a restart).
class ObFtsCompactRegistry
{
public:
  static ObFtsCompactRegistry &instance()
  {
    static ObFtsCompactRegistry inst;
    return inst;
  }
  void add(const uint64_t aux_table_id, const ObFtsCompactEntry &entry)
  {
    std::lock_guard<std::mutex> guard(mu_);
    entries_[aux_table_id] = entry;
  }
  bool get(const uint64_t aux_table_id, ObFtsCompactEntry &entry) const
  {
    bool found = false;
    std::lock_guard<std::mutex> guard(mu_);
    auto iter = entries_.find(aux_table_id);
    if (iter != entries_.end() && iter->second.is_valid()) {
      entry = iter->second;
      found = true;
    }
    return found;
  }
  void clear()
  {
    std::lock_guard<std::mutex> guard(mu_);
    entries_.clear();
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<uint64_t, ObFtsCompactEntry> entries_;
};

} // namespace sql
} // namespace oceanbase
