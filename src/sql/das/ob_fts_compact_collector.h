/**
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/mysqlclient/ob_isql_client.h"
#include "lib/ob_define.h"
#include "lib/ob_errno.h"
#include "lib/string/ob_sql_string.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace sql
{

// Process-wide collector for compact FTS postings produced by the DDL build
// insert path. Rows are grouped per target aux table and flushed to the
// compact posting table by the FTS DDL parent task.
class ObFtsCompactCollector
{
public:
  static const int64_t SHARD_CNT = 16;

  static const int64_t DOC_ID_SIZE = 16;
  struct Posting
  {
    uint8_t doc_id_[DOC_ID_SIZE];
    uint32_t tf_;
    uint32_t doc_len_;
  };

  static ObFtsCompactCollector &instance()
  {
    static ObFtsCompactCollector inst;
    return inst;
  }

  int add(const uint64_t aux_table_id,
          const common::ObString &word,
          const char *doc_id,
          const uint32_t tf,
          const uint32_t doc_len)
  {
    int ret = common::OB_SUCCESS;
    if (OB_UNLIKELY(common::OB_INVALID_ID == aux_table_id || word.empty() || OB_ISNULL(doc_id))) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      Shard &shard = shards_[std::hash<std::string>()(
          std::string(word.ptr(), word.length())) % SHARD_CNT];
      std::lock_guard<std::mutex> guard(shard.mu_);
      std::string key(word.ptr(), word.length());
      std::vector<Posting> &postings = shard.map_[key];
      Posting p;
      MEMCPY(p.doc_id_, doc_id, DOC_ID_SIZE);
      p.tf_ = tf;
      p.doc_len_ = doc_len;
      postings.push_back(p);
      {
        std::lock_guard<std::mutex> stats_guard(stats_mu_);
        doc_len_map_[std::string(doc_id, DOC_ID_SIZE)] = doc_len;
      }
    }
    return ret;
  }

  void clear(const uint64_t aux_table_id)
  {
    for (int64_t i = 0; i < SHARD_CNT; ++i) {
      std::lock_guard<std::mutex> guard(shards_[i].mu_);
      shards_[i].map_.clear();
    }
    std::lock_guard<std::mutex> stats_guard(stats_mu_);
    doc_len_map_.clear();
  }

  int flush(const uint64_t aux_table_id,
            common::ObISQLClient &proxy,
            const common::ObString &qualified_table_name,
            int64_t &total_doc_cnt,
            double &avg_doc_len)
  {
    int ret = common::OB_SUCCESS;
    std::lock_guard<std::mutex> meta_guard(meta_mu_);
    total_doc_cnt = 0;
    avg_doc_len = 0.0;
    {
      std::lock_guard<std::mutex> stats_guard(stats_mu_);
      total_doc_cnt = static_cast<int64_t>(doc_len_map_.size());
      int64_t sum_doc_len = 0;
      for (const auto &kv : doc_len_map_) {
        sum_doc_len += kv.second;
      }
      if (total_doc_cnt > 0) {
        avg_doc_len = static_cast<double>(sum_doc_len) / static_cast<double>(total_doc_cnt);
      }
      doc_len_map_.clear();
    }
    std::unordered_map<std::string, std::vector<Posting>> merged;
    for (int64_t i = 0; OB_SUCC(ret) && i < SHARD_CNT; ++i) {
      std::lock_guard<std::mutex> guard(shards_[i].mu_);
      for (auto &kv : shards_[i].map_) {
        std::vector<Posting> &dst = merged[kv.first];
        dst.insert(dst.end(), kv.second.begin(), kv.second.end());
      }
      shards_[i].map_.clear();
    }
    if (OB_SUCC(ret)) {
      common::ObSqlString sql;
      int64_t batch_rows = 0;
      int64_t batch_bytes = 0;
      auto flush_sql = [&]() -> int {
        int flush_ret = common::OB_SUCCESS;
        if (OB_LIKELY(batch_rows > 0)) {
          int64_t affected = 0;
          flush_ret = proxy.write(sql.ptr(), affected);
        }
        sql.reuse();
        batch_rows = 0;
        batch_bytes = 0;
        return flush_ret;
      };
      for (auto &kv : merged) {
        if (OB_FAIL(ret)) {
          break;
        }
        std::vector<Posting> &postings = kv.second;
        std::sort(postings.begin(), postings.end(),
                  [](const Posting &a, const Posting &b) {
                    return memcmp(a.doc_id_, b.doc_id_, DOC_ID_SIZE) < 0;
                  });
        // Encode the 24-byte posting array: 16-byte doc_id + tf + doc_len.
        const int64_t blob_len = static_cast<int64_t>(postings.size()) * (DOC_ID_SIZE + 8);
        std::vector<char> blob(blob_len);
        char *p = blob.data();
        for (const Posting &posting : postings) {
          MEMCPY(p, posting.doc_id_, DOC_ID_SIZE);
          MEMCPY(p + DOC_ID_SIZE, &posting.tf_, sizeof(uint32_t));
          MEMCPY(p + DOC_ID_SIZE + 4, &posting.doc_len_, sizeof(uint32_t));
          p += DOC_ID_SIZE + 8;
        }
        std::vector<char> hex(blob_len * 2 + 1);
        if (blob_len > 0
            && OB_FAIL(common::to_hex_cstr(blob.data(), blob_len, hex.data(), hex.size()))) {
          break;
        }
        if (0 == batch_rows && OB_FAIL(sql.append_fmt(
                "INSERT INTO %.*s (word, docs, doc_count) VALUES ",
                static_cast<int>(qualified_table_name.length()),
                qualified_table_name.ptr()))) {
          break;
        }
        if (OB_FAIL(sql.append(batch_rows > 0 ? ",(" : "("))) {
          break;
        }
        // Escape the word as a SQL string literal.
        if (OB_FAIL(sql.append("'"))) {
          break;
        }
        bool escape_ok = true;
        for (int64_t j = 0; j < static_cast<int64_t>(kv.first.length()) && escape_ok; ++j) {
          const char c = kv.first[j];
          if ('\'' == c) {
            escape_ok = OB_SUCC(sql.append("''"));
          } else if ('\\' == c) {
            escape_ok = OB_SUCC(sql.append("\\\\"));
          } else {
            char one[2] = {c, '\0'};
            escape_ok = OB_SUCC(sql.append(one, 1));
          }
        }
        if (!escape_ok) {
          ret = common::OB_ERR_UNEXPECTED;
          break;
        }
        if (OB_FAIL(sql.append_fmt("', X'%.*s', %ld)",
                                   static_cast<int>(blob_len * 2), hex.data(),
                                   static_cast<long>(postings.size())))) {
          break;
        }
        ++batch_rows;
        batch_bytes += blob_len * 2 + kv.first.length() + 32;
        if (batch_rows >= 200 || batch_bytes >= 1024 * 1024) {
          if (OB_FAIL(flush_sql())) {
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(flush_sql())) {
      }
    }
    return ret;
  }

private:
  struct Shard
  {
    std::mutex mu_;
    std::unordered_map<std::string, std::vector<Posting>> map_;
  };
  ObFtsCompactCollector() = default;
  Shard shards_[SHARD_CNT];
  std::mutex meta_mu_;
  std::mutex stats_mu_;
  std::unordered_map<std::string, uint32_t> doc_len_map_;
};

} // namespace sql
} // namespace oceanbase
