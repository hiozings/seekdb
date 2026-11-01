/**
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace storage
{

// One posting in the compact per-token posting list.
// The engine's doc_id is an opaque 16-byte index key, so a posting is
// (16-byte doc_id, uint32 tf, uint32 doc_len) = 24 bytes.
struct ObFtsCompactPosting
{
  static const int64_t DOC_ID_SIZE = 16;
  uint8_t doc_id_[DOC_ID_SIZE];
  uint32_t tf_;
  uint32_t doc_len_;

  ObFtsCompactPosting() : tf_(0), doc_len_(0) { MEMSET(doc_id_, 0, sizeof(doc_id_)); }
  ObFtsCompactPosting(const char *doc_id, const uint32_t tf, const uint32_t doc_len)
      : tf_(tf), doc_len_(doc_len)
  {
    MEMCPY(doc_id_, doc_id, DOC_ID_SIZE);
  }
  TO_STRING_KV(K_(tf), K_(doc_len));
};

class ObFtsCompactPostingCodec
{
public:
  static const int64_t POSTING_SIZE = ObFtsCompactPosting::DOC_ID_SIZE + 8;

  static int encode(const common::ObIArray<ObFtsCompactPosting> &postings,
                    common::ObIAllocator &allocator,
                    common::ObString &blob)
  {
    int ret = common::OB_SUCCESS;
    blob.reset();
    const int64_t size = postings.count() * POSTING_SIZE;
    if (size > 0) {
      char *buf = static_cast<char *>(allocator.alloc(size));
      if (OB_ISNULL(buf)) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        char *pos = buf;
        for (int64_t i = 0; i < postings.count(); ++i) {
          const ObFtsCompactPosting &p = postings.at(i);
          MEMCPY(pos, p.doc_id_, ObFtsCompactPosting::DOC_ID_SIZE);
          MEMCPY(pos + ObFtsCompactPosting::DOC_ID_SIZE, &p.tf_, sizeof(uint32_t));
          MEMCPY(pos + ObFtsCompactPosting::DOC_ID_SIZE + 4, &p.doc_len_, sizeof(uint32_t));
          pos += POSTING_SIZE;
        }
        blob.assign_ptr(buf, static_cast<common::ObString::obstr_size_t>(size));
      }
    }
    return ret;
  }

  static int decode(const common::ObString &blob,
                    common::ObIArray<ObFtsCompactPosting> &postings)
  {
    int ret = common::OB_SUCCESS;
    postings.reuse();
    if (blob.length() % POSTING_SIZE != 0) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      const char *pos = blob.ptr();
      const int64_t count = blob.length() / POSTING_SIZE;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        ObFtsCompactPosting p;
        MEMCPY(p.doc_id_, pos, ObFtsCompactPosting::DOC_ID_SIZE);
        MEMCPY(&p.tf_, pos + ObFtsCompactPosting::DOC_ID_SIZE, sizeof(uint32_t));
        MEMCPY(&p.doc_len_, pos + ObFtsCompactPosting::DOC_ID_SIZE + 4, sizeof(uint32_t));
        if (OB_FAIL(postings.push_back(p))) {
        }
        pos += POSTING_SIZE;
      }
    }
    return ret;
  }

  // Cardinality of the union of several doc_id-ascending posting lists.
  static int union_count(const common::ObIArray<const common::ObString *> &blobs,
                         int64_t &count)
  {
    int ret = common::OB_SUCCESS;
    count = 0;
    common::ObSEArray<int64_t, 8> positions;
    common::ObSEArray<int64_t, 8> counts;
    common::ObSEArray<ObFtsCompactPosting, 8> current;
    for (int64_t i = 0; OB_SUCC(ret) && i < blobs.count(); ++i) {
      const common::ObString *blob = blobs.at(i);
      if (OB_ISNULL(blob)) {
        ret = common::OB_INVALID_ARGUMENT;
      } else if (blob->length() % POSTING_SIZE != 0) {
        ret = common::OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(positions.push_back(0)) || OB_FAIL(counts.push_back(blob->length() / POSTING_SIZE))
                 || OB_FAIL(current.push_back(ObFtsCompactPosting()))) {
      } else if (counts.at(i) > 0) {
        MEMCPY(current.at(i).doc_id_, blob->ptr(), ObFtsCompactPosting::DOC_ID_SIZE);
      }
    }
    while (OB_SUCC(ret)) {
      const uint8_t *min_id = nullptr;
      for (int64_t i = 0; i < blobs.count(); ++i) {
        if (positions.at(i) < counts.at(i)
            && (nullptr == min_id
                || 0 > memcmp(current.at(i).doc_id_, min_id, ObFtsCompactPosting::DOC_ID_SIZE))) {
          min_id = current.at(i).doc_id_;
        }
      }
      if (nullptr == min_id) {
        break;
      }
      uint8_t min_copy[ObFtsCompactPosting::DOC_ID_SIZE];
      MEMCPY(min_copy, min_id, ObFtsCompactPosting::DOC_ID_SIZE);
      ++count;
      for (int64_t i = 0; i < blobs.count(); ++i) {
        if (positions.at(i) < counts.at(i)
            && 0 == memcmp(current.at(i).doc_id_, min_copy, ObFtsCompactPosting::DOC_ID_SIZE)) {
          ++positions.at(i);
          if (positions.at(i) < counts.at(i)) {
            const char *p = blobs.at(i)->ptr() + positions.at(i) * POSTING_SIZE;
            MEMCPY(current.at(i).doc_id_, p, ObFtsCompactPosting::DOC_ID_SIZE);
          }
        }
      }
    }
    return ret;
  }
};

} // namespace storage
} // namespace oceanbase
