/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_DAS
#include "observer/composition/retrieval/ob_das_legacy_tr_merge_iter.h"
#include "sql/das/iter/ob_das_scan_iter.h"
#include "sql/das/iter/ob_das_text_retrieval_eval_node.h"
#include "sql/das/iter/sparse_retrieval/ob_das_text_retrieval_engine.h"
#include "sql/das/ob_das_ir_define.h"
#include "sql/das/ob_fts_compact_registry.h"
#include "storage/fts/ob_fts_compact_posting.h"
#include "storage/tx_storage/ob_access_service.h"
#include "storage/access/ob_table_scan_iterator.h"
#include "storage/access/ob_table_param.h"
#include "storage/access/ob_dml_param.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "config_bridge.h"
#include <cstring>
#include <cmath>

namespace oceanbase
{
using namespace share;
namespace sql
{

namespace
{

// Reader for one compact posting row (word -> 24-byte postings blob).
class ObFtsCompactScanReader
{
public:
  explicit ObFtsCompactScanReader(common::ObIAllocator &alloc)
      : allocator_(alloc),
        table_alloc_(lib::ObMemAttr("FtCompactTbl")),
        table_param_(table_alloc_),
        scan_allocator_(lib::ObMemAttr("FtCompactScan")),
        iter_(nullptr),
        table_id_(common::OB_INVALID_ID),
        word_collation_(common::CS_TYPE_INVALID),
        is_inited_(false)
  {
  }
  ~ObFtsCompactScanReader() { reset(); }

  void reset()
  {
    if (OB_NOT_NULL(iter_)) {
      ::oceanbase::share::server_service<::oceanbase::storage::ObAccessService>()->revert_scan_iter(iter_);
      iter_ = nullptr;
    }
    table_param_.reset();
    scan_allocator_.reset();
    is_inited_ = false;
  }

  int init(const ObFtsCompactEntry &entry, const transaction::ObTxReadSnapshot *snapshot)
  {
    int ret = common::OB_SUCCESS;
    share::schema::ObSchemaGetterGuard guard;
    const ObTableSchema *schema = nullptr;
    common::ObSEArray<uint64_t, 4> column_ids;
    if (OB_UNLIKELY(!entry.is_valid() || OB_ISNULL(snapshot))) {
      ret = common::OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(share::schema::ObMultiVersionSchemaService::get_instance().get_runtime_schema_guard(guard))) {
    } else if (OB_FAIL(guard.get_table_schema(entry.table_id_, schema))) {
    } else if (OB_ISNULL(schema)) {
      ret = common::OB_TABLE_NOT_EXIST;
    } else if (OB_FAIL(schema->get_column_ids(column_ids))) {
    } else if (OB_FAIL(table_param_.convert(*schema, column_ids, ObStoragePushdownFlag()))) {
    } else if (OB_FAIL(scan_param_.column_ids_.assign(column_ids))) {
    } else {
      const ObColumnSchemaV2 *word_col = schema->get_column_schema(entry.word_col_id_);
      if (OB_ISNULL(word_col)) {
        ret = common::OB_ERR_UNEXPECTED;
      } else if (FALSE_IT(word_collation_ = word_col->get_collation_type())) {
      } else if (FALSE_IT(table_id_ = entry.table_id_)) {
      } else {
        ObQueryFlag query_flag(ObQueryFlag::Forward, false, false, false, false, false, false, false);
        scan_param_.tablet_id_ = entry.tablet_id_;
        scan_param_.schema_version_ = entry.schema_version_;
        scan_param_.is_get_ = false;
        scan_param_.scan_flag_.flag_ = query_flag.flag_;
        scan_param_.key_ranges_.set_attr(common::ObMemAttr("FtCompactKR"));
        scan_param_.index_id_ = 0;
        scan_param_.timeout_ = THIS_WORKER.get_timeout_ts();
        scan_param_.reserved_cell_count_ = scan_param_.column_ids_.count();
        scan_param_.limit_param_.limit_ = -1;
        scan_param_.limit_param_.offset_ = 0;
        scan_param_.sql_mode_ = SMO_DEFAULT;
        scan_param_.allocator_ = &allocator_;
        scan_param_.for_update_ = false;
        scan_param_.for_update_wait_timeout_ = scan_param_.timeout_;
        scan_param_.scan_allocator_ = &scan_allocator_;
        scan_param_.frozen_version_ = -1;
        scan_param_.output_exprs_ = nullptr;
        scan_param_.aggregate_exprs_ = nullptr;
        scan_param_.op_ = nullptr;
        scan_param_.row2exprs_projector_ = nullptr;
        scan_param_.need_scn_ = false;
        scan_param_.pd_storage_flag_ = false;
        scan_param_.table_param_ = &table_param_;
        if (OB_FAIL(scan_param_.snapshot_.assign(*snapshot))) {
        } else {
          scan_param_.tx_id_ = scan_param_.snapshot_.core_.tx_id_;
          is_inited_ = true;
        }
      }
    }
    return ret;
  }

  int fetch(const common::ObString &token, common::ObString &docs)
  {
    int ret = common::OB_SUCCESS;
    docs.reset();
    if (OB_UNLIKELY(!is_inited_)) {
      ret = common::OB_NOT_INIT;
    } else if (OB_NOT_NULL(iter_)) {
      if (OB_FAIL(::oceanbase::share::server_service<::oceanbase::storage::ObAccessService>()->revert_scan_iter(iter_))) {
      } else {
        iter_ = nullptr;
      }
    }
    if (OB_SUCC(ret)) {
      common::ObObj *objs = static_cast<common::ObObj *>(allocator_.alloc(sizeof(common::ObObj) * 2));
      if (OB_ISNULL(objs)) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        objs[0].set_varchar(token);
        objs[0].set_collation_type(word_collation_);
        objs[1] = objs[0];
        common::ObNewRange range;
        range.table_id_ = table_id_;
        range.start_key_ = common::ObRowkey(&objs[0], 1);
        range.end_key_ = common::ObRowkey(&objs[1], 1);
        range.border_flag_.set_inclusive_start();
        range.border_flag_.set_inclusive_end();
        if (OB_FAIL(scan_param_.key_ranges_.push_back(range))) {
        } else if (OB_FAIL(::oceanbase::share::server_service<::oceanbase::storage::ObAccessService>()->table_scan(scan_param_, iter_))) {
        } else {
          ObTableScanIterator *scan_iter = static_cast<ObTableScanIterator *>(iter_);
          blocksstable::ObDatumRow *row = nullptr;
          if (OB_FAIL(scan_iter->get_next_row(row))) {
            if (common::OB_ITER_END == ret) {
              ret = common::OB_ENTRY_NOT_EXIST;
            }
          } else if (OB_ISNULL(row) || row->get_column_count() < 2) {
            ret = common::OB_ERR_UNEXPECTED;
          } else {
            docs = row->storage_datums_[1].get_string();
          }
        }
        scan_param_.key_ranges_.reuse();
      }
    }
    return ret;
  }

  int fetch_multi(const common::ObIArray<common::ObString> &tokens,
                  common::ObIArray<common::ObString> &docs)
  {
    int ret = common::OB_SUCCESS;
    docs.reuse();
    if (OB_UNLIKELY(!is_inited_)) {
      ret = common::OB_NOT_INIT;
    } else if (OB_NOT_NULL(iter_)) {
      if (OB_FAIL(::oceanbase::share::server_service<::oceanbase::storage::ObAccessService>()->revert_scan_iter(iter_))) {
      } else {
        iter_ = nullptr;
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < tokens.count(); ++i) {
      if (OB_FAIL(docs.push_back(common::ObString()))) {
      }
    }
    if (OB_SUCC(ret)) {
      scan_param_.key_ranges_.reuse();
      for (int64_t i = 0; OB_SUCC(ret) && i < tokens.count(); ++i) {
        common::ObObj *objs = static_cast<common::ObObj *>(allocator_.alloc(sizeof(common::ObObj) * 2));
        if (OB_ISNULL(objs)) {
          ret = common::OB_ALLOCATE_MEMORY_FAILED;
        } else {
          objs[0].set_varchar(tokens.at(i));
          objs[0].set_collation_type(word_collation_);
          objs[1] = objs[0];
          common::ObNewRange range;
          range.table_id_ = table_id_;
          range.start_key_ = common::ObRowkey(&objs[0], 1);
          range.end_key_ = common::ObRowkey(&objs[1], 1);
          range.border_flag_.set_inclusive_start();
          range.border_flag_.set_inclusive_end();
          ret = scan_param_.key_ranges_.push_back(range);
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(::oceanbase::share::server_service<::oceanbase::storage::ObAccessService>()->table_scan(scan_param_, iter_))) {
      } else {
        ObTableScanIterator *scan_iter = static_cast<ObTableScanIterator *>(iter_);
        blocksstable::ObDatumRow *row = nullptr;
        while (OB_SUCC(ret)) {
          ret = scan_iter->get_next_row(row);
          if (common::OB_ITER_END == ret) {
            ret = common::OB_SUCCESS;
            break;
          } else if (OB_FAIL(ret)) {
          } else if (OB_ISNULL(row) || row->get_column_count() < 2) {
            ret = common::OB_ERR_UNEXPECTED;
          } else {
            const common::ObString word = row->storage_datums_[0].get_string();
            for (int64_t i = 0; i < tokens.count(); ++i) {
              if (word.length() == tokens.at(i).length()
                  && 0 == memcmp(word.ptr(), tokens.at(i).ptr(), word.length())) {
                common::ObString copied;
                if (OB_FAIL(common::ob_write_string(allocator_, row->storage_datums_[1].get_string(), copied))) {
                } else {
                  docs.at(i) = copied;
                }
                break;
              }
            }
          }
        }
      }
    }
    if (OB_NOT_NULL(iter_)) {
      int tmp_ret = ::oceanbase::share::server_service<::oceanbase::storage::ObAccessService>()->revert_scan_iter(iter_);
      if (common::OB_SUCCESS != tmp_ret && common::OB_SUCCESS == ret) {
        ret = tmp_ret;
      }
      iter_ = nullptr;
    }
    scan_param_.key_ranges_.reuse();
    return ret;
  }

private:
  common::ObIAllocator &allocator_;
  common::ObArenaAllocator table_alloc_;
  share::schema::ObTableParam table_param_;
  common::ObArenaAllocator scan_allocator_;
  storage::ObTableScanParam scan_param_;
  common::ObNewRowIterator *iter_;
  uint64_t table_id_;
  common::ObCollationType word_collation_;
  bool is_inited_;
};

// One compact dimension iterator: yields decoded postings for a query token.
class ObFtsCompactDimIter final : public ObISRDaaTDimIter
{
public:
  explicit ObFtsCompactDimIter(common::ObIAllocator &alloc)
      : alloc_(alloc), postings_(), pos_(0), score_(1.0), reader_(alloc),
        total_doc_cnt_(0), avg_doc_len_(0.0)
  {
  }
  ~ObFtsCompactDimIter() override = default;

  int init(const ObFtsCompactEntry &entry,
           const common::ObString &token,
           const transaction::ObTxReadSnapshot *snapshot)
  {
    int ret = common::OB_SUCCESS;
    common::ObString docs;
    total_doc_cnt_ = entry.total_doc_cnt_;
    avg_doc_len_ = entry.avg_doc_len_;
    if (OB_FAIL(reader_.init(entry, snapshot))) {
    } else if (OB_FAIL(reader_.fetch(token, docs))) {
      if (common::OB_ENTRY_NOT_EXIST == ret) {
        ret = common::OB_SUCCESS;
      }
    } else if (OB_FAIL(storage::ObFtsCompactPostingCodec::decode(docs, postings_))) {
    }
    return ret;
  }

  int init_from_docs(const ObFtsCompactEntry &entry, const common::ObString &docs)
  {
    int ret = common::OB_SUCCESS;
    total_doc_cnt_ = entry.total_doc_cnt_;
    avg_doc_len_ = entry.avg_doc_len_;
    if (!docs.empty() && OB_FAIL(storage::ObFtsCompactPostingCodec::decode(docs, postings_))) {
    }
    return ret;
  }

  int get_next_row() override
  {
    int ret = common::OB_SUCCESS;
    if (pos_ >= postings_.count()) {
      ret = common::OB_ITER_END;
    } else {
      cur_id_.set_string(reinterpret_cast<const char *>(postings_.at(pos_).doc_id_),
                         storage::ObFtsCompactPosting::DOC_ID_SIZE);
      score_ = score_at(pos_);
      ++pos_;
    }
    return ret;
  }

  int get_next_batch(const int64_t capacity, int64_t &count) override
  {
    int ret = common::OB_SUCCESS;
    count = 0;
    while (OB_SUCC(ret) && count < capacity) {
      if (OB_FAIL(get_next_row())) {
        if (common::OB_ITER_END == ret) {
          ret = common::OB_SUCCESS;
        }
      } else {
        ++count;
      }
    }
    return ret;
  }

  int advance_to(const common::ObDatum &id_datum) override
  {
    int ret = common::OB_SUCCESS;
    const common::ObString id = id_datum.get_string();
    if (OB_UNLIKELY(storage::ObFtsCompactPosting::DOC_ID_SIZE != id.length())) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      int64_t lo = 0;
      int64_t hi = postings_.count();
      while (lo < hi) {
        const int64_t mid = (lo + hi) / 2;
        if (0 > memcmp(postings_.at(mid).doc_id_, id.ptr(), storage::ObFtsCompactPosting::DOC_ID_SIZE)) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      pos_ = lo;
      if (pos_ < postings_.count()) {
        score_ = score_at(pos_);
      }
    }
    return ret;
  }

  int get_curr_score(double &score) const override
  {
    score = score_;
    return common::OB_SUCCESS;
  }

  int get_curr_id(const common::ObDatum *&id_datum) const override
  {
    int ret = common::OB_SUCCESS;
    if (pos_ == 0 || pos_ > postings_.count()) {
      ret = common::OB_ARRAY_OUT_OF_RANGE;
    } else {
      id_datum = &cur_id_;
    }
    return ret;
  }

  int get_dim_max_score(double &score) override
  {
    score = score_;
    return common::OB_SUCCESS;
  }

  const common::ObSEArray<storage::ObFtsCompactPosting, 64> &get_postings() const { return postings_; }

  double score_at(const int64_t idx) const
  {
    double score = 0.0;
    if (idx >= 0 && idx < postings_.count()) {
      const storage::ObFtsCompactPosting &p = postings_.at(idx);
      const double df = static_cast<double>(postings_.count());
      const double total = static_cast<double>(total_doc_cnt_ > 0 ? total_doc_cnt_ : 1);
      const double diff = (total - df) > 0 ? (total - df) : 0;
      const double idf = std::log((diff + 0.5) / (df + 0.5));
      const double token_weight = (idf > 0.25 ? idf : 0.25) * 2.2;
      const double avg = avg_doc_len_ > 0.0 ? avg_doc_len_ : 10.0;
      const double norm = static_cast<double>(p.doc_len_) / avg;
      const double doc_weight = static_cast<double>(p.tf_)
          / (static_cast<double>(p.tf_) + 1.2 * (1.0 - 0.75 + 0.75 * norm));
      score = token_weight * doc_weight;
    }
    return score;
  }

  void reuse() override { pos_ = 0; }
  void reset() override
  {
    postings_.reuse();
    pos_ = 0;
    reader_.reset();
  }

private:
  common::ObIAllocator &alloc_;
  common::ObSEArray<storage::ObFtsCompactPosting, 64> postings_;
  int64_t pos_;
  double score_;
  common::ObDatum cur_id_;
  ObFtsCompactScanReader reader_;
  int64_t total_doc_cnt_;
  double avg_doc_len_;
};

struct ObFtsCompactResult
{
  uint8_t doc_id_[storage::ObFtsCompactPosting::DOC_ID_SIZE];
  double score_;
};

// Bulk merge for compact postings: precomputes the full match set once and
// then projects it in batches, avoiding the generic row-wise DAAT heap.
class ObFtsCompactMergeIter final : public storage::ObISparseRetrievalMergeIter
{
public:
  explicit ObFtsCompactMergeIter(common::ObIAllocator &alloc)
      : alloc_(alloc), results_(), pos_(0), prepared_(false), param_(nullptr),
        dim_iters_(nullptr), should_match_(1), score_(1.0), skip_projection_(false),
        count_only_count_(0)
  {
  }
  ~ObFtsCompactMergeIter() override = default;

  int init(const ObSparseRetrievalMergeParam *param,
           const common::ObIArray<ObISRDaaTDimIter *> *dim_iters,
           const int64_t should_match)
  {
    int ret = common::OB_SUCCESS;
    if (OB_ISNULL(param) || OB_ISNULL(dim_iters)) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      param_ = param;
      dim_iters_ = dim_iters;
      should_match_ = should_match > 0 ? should_match : 1;
    }
    return ret;
  }

  int get_next_row() override
  {
    int ret = common::OB_SUCCESS;
    int64_t count = 0;
    if (OB_FAIL(prepare())) {
    } else if (pos_ >= results_.count()) {
      ret = common::OB_ITER_END;
    } else if (OB_FAIL(project(1, count))) {
    } else {
      ++pos_;
    }
    return ret;
  }

  int get_next_rows(const int64_t capacity, int64_t &count) override
  {
    int ret = common::OB_SUCCESS;
    count = 0;
    if (OB_FAIL(prepare())) {
    } else {
      const int64_t remain = results_.count() - pos_;
      const int64_t batch = MIN(capacity, remain);
      if (batch <= 0) {
        ret = common::OB_ITER_END;
      } else if (OB_FAIL(project(batch, count))) {
      } else {
        pos_ += count;
      }
    }
    return ret;
  }

  void reuse(const bool switch_tablet = false) override
  {
    UNUSED(switch_tablet);
    pos_ = 0;
  }
  void reset() override
  {
    results_.reuse();
    pos_ = 0;
    prepared_ = false;
  }
  void set_skip_projection(const bool v) { skip_projection_ = v; }

  int get_match_count(int64_t &count)
  {
    int ret = common::OB_SUCCESS;
    count = 0;
    if (prepared_) {
      count = results_.count();
    } else if (OB_FAIL(prepare(true))) {
    } else {
      count = count_only_count_;
    }
    return ret;
  }

  int get_query_max_score(double &score) override
  {
    int ret = common::OB_SUCCESS;
    if (OB_FAIL(prepare())) {
    } else {
      score = results_.empty() ? 0.0 : results_.at(0).score_;
    }
    return ret;
  }

private:
  int prepare(const bool count_only = false)
  {
    int ret = common::OB_SUCCESS;
    if (prepared_) {
    } else if (OB_ISNULL(param_) || OB_ISNULL(param_->eval_ctx_) || OB_ISNULL(param_->id_proj_expr_)) {
      ret = common::OB_ERR_UNEXPECTED;
    } else {
      if (count_only) {
        count_only_count_ = 0;
      }
      const int64_t dim_cnt = dim_iters_->count();
      common::ObSEArray<int64_t, 8> positions;
      common::ObSEArray<int64_t, 8> counts;
      common::ObSEArray<common::ObString, 8> current;
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_cnt; ++i) {
        const ObFtsCompactDimIter *dim = static_cast<const ObFtsCompactDimIter *>(dim_iters_->at(i));
        const auto &postings = dim->get_postings();
        if (OB_FAIL(positions.push_back(0)) || OB_FAIL(counts.push_back(postings.count()))
            || OB_FAIL(current.push_back(common::ObString()))) {
        } else if (!postings.empty()) {
          current.at(i).assign_ptr(reinterpret_cast<const char *>(postings.at(0).doc_id_),
                                   storage::ObFtsCompactPosting::DOC_ID_SIZE);
        }
      }
      while (OB_SUCC(ret)) {
        const char *min_id = nullptr;
        for (int64_t i = 0; i < dim_cnt; ++i) {
          if (positions.at(i) < counts.at(i)
              && (nullptr == min_id || 0 > memcmp(current.at(i).ptr(), min_id,
                                                  storage::ObFtsCompactPosting::DOC_ID_SIZE))) {
            min_id = current.at(i).ptr();
          }
        }
        if (nullptr == min_id) {
          break;
        }
        int64_t matched = 0;
        double total_score = 0.0;
        for (int64_t i = 0; i < dim_cnt; ++i) {
          if (positions.at(i) < counts.at(i)
              && 0 == memcmp(current.at(i).ptr(), min_id,
                             storage::ObFtsCompactPosting::DOC_ID_SIZE)) {
            ++matched;
            if (!count_only) {
              total_score += static_cast<const ObFtsCompactDimIter *>(dim_iters_->at(i))->score_at(positions.at(i));
            }
            ++positions.at(i);
            if (positions.at(i) < counts.at(i)) {
              const ObFtsCompactDimIter *dim = static_cast<const ObFtsCompactDimIter *>(dim_iters_->at(i));
              const auto &postings = dim->get_postings();
              current.at(i).assign_ptr(reinterpret_cast<const char *>(postings.at(positions.at(i)).doc_id_),
                                       storage::ObFtsCompactPosting::DOC_ID_SIZE);
            }
          }
        }
        if (matched >= should_match_) {
          if (count_only) {
            ++count_only_count_;
          } else {
            ObFtsCompactResult result;
            MEMCPY(result.doc_id_, min_id, storage::ObFtsCompactPosting::DOC_ID_SIZE);
            result.score_ = total_score;
            if (OB_FAIL(results_.push_back(result))) {
            }
          }
        }
      }
      if (OB_SUCC(ret) && !count_only && param_->topk_limit_ > 0 && results_.count() > param_->topk_limit_) {
        std::sort(results_.begin(), results_.end(),
                  [](const ObFtsCompactResult &a, const ObFtsCompactResult &b) {
                    if (a.score_ != b.score_) {
                      return a.score_ > b.score_;
                    }
                    return 0 > memcmp(a.doc_id_, b.doc_id_, storage::ObFtsCompactPosting::DOC_ID_SIZE);
                  });
        while (results_.count() > param_->topk_limit_) {
          results_.pop_back();
        }
      }
      if (OB_SUCC(ret) && !count_only) {
        prepared_ = true;
        pos_ = 0;
      }
    }
    return ret;
  }

  int project(const int64_t count, int64_t &projected)
  {
    int ret = common::OB_SUCCESS;
    projected = 0;
    ObEvalCtx *eval_ctx = param_->eval_ctx_;
    ObExpr *id_expr = param_->id_proj_expr_;
    if (OB_UNLIKELY(count <= 0 || pos_ + count > results_.count())) {
      ret = common::OB_INVALID_ARGUMENT;
    } else if (skip_projection_) {
      projected = count;
    } else if (eval_ctx->is_vectorized()) {
      ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
      sql::ObBitVector &id_flags = id_expr->get_evaluated_flags(*eval_ctx);
      ObExpr *relevance_expr = param_->relevance_proj_expr_;
      sql::ObBitVector *relevance_flags = nullptr;
      if (OB_NOT_NULL(relevance_expr)) {
        relevance_flags = &relevance_expr->get_evaluated_flags(*eval_ctx);
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        const ObFtsCompactResult &result = results_.at(pos_ + i);
        guard.set_batch_idx(i);
        ObDatum &id_datum = id_expr->locate_datum_for_write(*eval_ctx);
        id_datum.set_string(reinterpret_cast<const char *>(result.doc_id_),
                            storage::ObFtsCompactPosting::DOC_ID_SIZE);
        id_flags.set(i);
        id_expr->set_evaluated_projected(*eval_ctx);
        if (OB_NOT_NULL(relevance_expr)) {
          ObDatum &relevance_datum = relevance_expr->locate_datum_for_write(*eval_ctx);
          relevance_datum.set_double(result.score_);
          relevance_flags->set(i);
          relevance_expr->set_evaluated_projected(*eval_ctx);
        }
      }
      projected = count;
    } else if (1 == count) {
      ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
      guard.set_batch_idx(0);
      ObDatum &id_datum = id_expr->locate_datum_for_write(*eval_ctx);
      id_datum.set_string(reinterpret_cast<const char *>(results_.at(pos_).doc_id_),
                          storage::ObFtsCompactPosting::DOC_ID_SIZE);
      id_expr->set_evaluated_projected(*eval_ctx);
      if (OB_NOT_NULL(param_->relevance_proj_expr_)) {
        ObDatum &relevance_datum = param_->relevance_proj_expr_->locate_datum_for_write(*eval_ctx);
        relevance_datum.set_double(results_.at(pos_).score_);
        param_->relevance_proj_expr_->set_evaluated_projected(*eval_ctx);
      }
      projected = 1;
    } else {
      ret = common::OB_ERR_UNEXPECTED;
    }
    return ret;
  }

private:
  common::ObIAllocator &alloc_;
  common::ObSEArray<ObFtsCompactResult, 512> results_;
  int64_t pos_;
  bool prepared_;
  const ObSparseRetrievalMergeParam *param_;
  const common::ObIArray<ObISRDaaTDimIter *> *dim_iters_;
  int64_t should_match_;
  double score_;
  bool skip_projection_;
  int64_t count_only_count_;
};

} // namespace

ObDASLegacyTRMergeIter::ObDASLegacyTRMergeIter()
  : ObDASIter(ObDASIterType::DAS_ITER_TEXT_RETRIEVAL_MERGE),
    mem_context_(nullptr),
    myself_allocator_(lib::ObMemAttr("SRMergeIterSelf"), OB_MALLOC_NORMAL_BLOCK_SIZE),
    ir_ctdef_(nullptr),
    ir_rtdef_(nullptr),
    tx_desc_(nullptr),
    snapshot_(nullptr),
    query_tokens_(),
    dim_weights_(),
    sr_iter_param_(),
    sparse_retrieval_iter_(nullptr),
    dim_iters_(),
    dim_iter_(),
    total_doc_cnt_scan_param_(nullptr),
    inv_scan_params_(),
    inv_agg_params_(),
    boolean_compute_node_(nullptr),
    block_max_scan_params_(),
    block_max_iter_param_(),
    doc_length_est_param_(),
    doc_length_est_stat_cols_(),
    topk_limit_(0),
    total_doc_cnt_tablet_id_(),
    inv_idx_tablet_id_(),
    flags_(0),
    check_rangekey_inited_(false),
    inv_idx_tablet_switched_(false),
    use_compact_fts_(false),
    skip_projection_(false),
    compact_entry_(),
    is_inited_(false)
{
}

int ObDASLegacyTRMergeIter::inner_init(ObDASIterParam &param)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(ObDASIterType::DAS_ITER_TEXT_RETRIEVAL_MERGE != param.type_)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObDASTRMergeIterParam &merge_param = static_cast<ObDASTRMergeIterParam &>(param);
    ir_ctdef_ = merge_param.ir_ctdef_;
    ir_rtdef_ = merge_param.ir_rtdef_;
    tx_desc_ = merge_param.tx_desc_;
    snapshot_ = merge_param.snapshot_;
    boolean_compute_node_ = merge_param.boolean_compute_node_;
    flags_ = merge_param.flags_;
    for (int64_t i = 0; OB_SUCC(ret) && i < merge_param.query_tokens_.count(); ++i) {
      if (OB_FAIL(query_tokens_.push_back(merge_param.query_tokens_.at(i)))) {
      }
    }
    if (merge_param.dim_weights_.count() >= 1) {
      for (int64_t i = 0; OB_SUCC(ret) && i < merge_param.dim_weights_.count(); ++i) {
        if (OB_FAIL(dim_weights_.push_back(merge_param.dim_weights_.at(i)))) {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(ir_ctdef_) || OB_ISNULL(ir_rtdef_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (daat_mode_ && !topk_mode_ && !function_lookup_mode_
               && config::_enable_fts_posting_index()
               && OB_NOT_NULL(ir_ctdef_->get_inv_idx_scan_ctdef())) {
      use_compact_fts_ = ObFtsCompactRegistry::instance().get(
          ir_ctdef_->get_inv_idx_scan_ctdef()->ref_table_id_, compact_entry_);
    }
    if (OB_FAIL(ret)) {
    } else if (topk_mode_ && OB_FAIL(init_topk_limit())) {
    } else if (OB_ISNULL(mem_context_)) {
      lib::ContextParam mem_param;
      mem_param.set_mem_attr("TextMergeIter", ObCtxIds::DEFAULT_CTX_ID);
      if (OB_FAIL(CURRENT_CONTEXT->CREATE_CONTEXT(mem_context_, mem_param))) {
      }
    }
    sr_iter_param_.max_batch_size_ = merge_param.max_batch_size_; // may be greater than ir_rtdef_->eval_ctx_->max_batch_size_
    inv_idx_tablet_switched_ = false;
    is_inited_ = true;
  }
  return ret;
}

int ObDASLegacyTRMergeIter::get_match_count(int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  if (!use_compact_fts_) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_ISNULL(sparse_retrieval_iter_)) {
    if (dim_iters_.empty() && OB_FAIL(create_dim_iters())) {
    } else if (OB_FAIL(create_sparse_retrieval_iter())) {
    } else {
      check_rangekey_inited_ = true;
    }
  }
  if (OB_SUCC(ret)) {
    ObFtsCompactMergeIter *compact_iter = static_cast<ObFtsCompactMergeIter *>(sparse_retrieval_iter_);
    ret = compact_iter->get_match_count(count);
  }
  return ret;
}

int ObDASLegacyTRMergeIter::init_das_iter_scan_params()
{
  int ret = OB_SUCCESS;
  const int64_t dim_iter_cnt = taat_mode_ ? 1 : query_tokens_.count();
  if (ir_ctdef_->need_calc_relevance() && !ir_ctdef_->need_estimate_total_doc_cnt() && 0 != dim_iter_cnt) {
    void *buf = nullptr;
    if (OB_ISNULL(buf = myself_allocator_.alloc(sizeof(ObTableScanParam)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      total_doc_cnt_scan_param_ = new (buf) ObTableScanParam();
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(init_das_iter_scan_param(
        total_doc_cnt_tablet_id_,
        ir_ctdef_->get_doc_agg_ctdef(),
        ir_rtdef_->get_doc_agg_rtdef(),
        tx_desc_, snapshot_, mem_context_->get_arena_allocator(),
        *total_doc_cnt_scan_param_))) {
    } else {
      static_cast<ObDASScanIter*>(children_[children_cnt_ - 1])->set_scan_param(*total_doc_cnt_scan_param_);
    }
  }

  if (OB_SUCC(ret) && 0 != dim_iter_cnt) {
    if (FALSE_IT(inv_scan_params_.set_allocator(&myself_allocator_))) {
    } else if (OB_FAIL(inv_scan_params_.init(dim_iter_cnt))) {
    } else if (OB_FAIL(inv_scan_params_.prepare_allocate(dim_iter_cnt))) {
    }

    if (OB_FAIL(ret) || !ir_ctdef_->need_inv_idx_agg()) {
    } else if (FALSE_IT(inv_agg_params_.set_allocator(&myself_allocator_))) {
    } else if (OB_FAIL(inv_agg_params_.init(dim_iter_cnt))) {
    } else if (OB_FAIL(inv_agg_params_.prepare_allocate(dim_iter_cnt))) {
    }

    if (OB_SUCC(ret)) {
      void *inv_buf = nullptr;
      if (OB_ISNULL(inv_buf = myself_allocator_.alloc(sizeof(ObTableScanParam) * dim_iter_cnt))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ObTableScanParam *scan_params = static_cast<ObTableScanParam*>(inv_buf);
        for (int64_t i = 0; OB_SUCC(ret) && i < dim_iter_cnt; ++i) {
          inv_scan_params_[i] = new (&scan_params[i]) ObTableScanParam();
          if (OB_FAIL(init_das_iter_scan_param(
              inv_idx_tablet_id_,
              ir_ctdef_->get_inv_idx_scan_ctdef(), ir_rtdef_->get_inv_idx_scan_rtdef(),
              tx_desc_, snapshot_, mem_context_->get_arena_allocator(),
              *inv_scan_params_[i]))) {
          } else {
            static_cast<ObDASScanIter*>(children_[i])->set_scan_param(*inv_scan_params_[i]);
          }
        }
      }

      if (OB_SUCC(ret) && ir_ctdef_->need_inv_idx_agg()) {
        void *agg_buf = nullptr;
        if (OB_ISNULL(agg_buf = myself_allocator_.alloc(sizeof(ObTableScanParam) * dim_iter_cnt))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          ObTableScanParam *agg_params = static_cast<ObTableScanParam*>(agg_buf);
          for (int64_t i = 0; OB_SUCC(ret) && i < dim_iter_cnt; ++i) {
            inv_agg_params_[i] = new (&agg_params[i]) ObTableScanParam();
            if (OB_FAIL(init_das_iter_scan_param(
                inv_idx_tablet_id_,
                ir_ctdef_->get_inv_idx_agg_ctdef(),
                ir_rtdef_->get_inv_idx_agg_rtdef(),
                tx_desc_, snapshot_, mem_context_->get_arena_allocator(),
                *inv_agg_params_[i]))) {
            } else {
              static_cast<ObDASScanIter*>(children_[i + dim_iter_cnt])->set_scan_param(*inv_agg_params_[i]);
            }
            inv_scan_params_[i]->scan_flag_.scan_order_ = ObQueryFlag::Forward;
          }
        }
      }

    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::init_block_max_iter_param()
{
  int ret = OB_SUCCESS;
  const int64_t token_cnt = query_tokens_.count();
  block_max_scan_params_.set_allocator(&myself_allocator_);
  if (0 == token_cnt) {
    // do nothing
  } else if (OB_FAIL(block_max_iter_param_.init(*ir_ctdef_, myself_allocator_))) {
  } else if (OB_FAIL(block_max_scan_params_.init(token_cnt))) {
  } else if (OB_FAIL(block_max_scan_params_.prepare_allocate(token_cnt))) {
  } else {
    ObTableScanParam *scan_params = nullptr;
    if (OB_ISNULL(scan_params = static_cast<ObTableScanParam *>(myself_allocator_.alloc(sizeof(ObTableScanParam) * token_cnt)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < token_cnt; ++i) {
      block_max_scan_params_[i] = new (&scan_params[i]) ObTableScanParam();
      if (OB_FAIL(init_das_iter_scan_param(
          inv_idx_tablet_id_,
          ir_ctdef_->get_block_max_scan_ctdef(),
          ir_rtdef_->get_block_max_scan_rtdef(),
          tx_desc_, snapshot_, mem_context_->get_arena_allocator(),
          *block_max_scan_params_[i]))) {
      }
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::init_doc_length_est_param()
{
  int ret = OB_SUCCESS;
  const ObTextAvgDocLenEstSpec &doc_len_est_spec = ir_ctdef_->avg_doc_len_est_spec_;
  doc_length_est_stat_cols_.set_allocator(&myself_allocator_);
  if (!ir_ctdef_->need_avg_doc_len_est() || !doc_len_est_spec.can_est_by_sum_skip_index_) {
    // skip
  } else if (OB_UNLIKELY(!doc_len_est_spec.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(doc_length_est_stat_cols_.init(doc_len_est_spec.col_types_.count()))) {
  } else if (OB_FAIL(doc_length_est_stat_cols_.push_back(
      ObSkipIndexColMeta(doc_len_est_spec.col_store_idxes_.at(0), doc_len_est_spec.col_types_.at(0))))) {
  } else if (OB_FAIL(doc_length_est_param_.init(
      doc_length_est_stat_cols_,
      doc_len_est_spec.scan_col_proj_,
      *inv_scan_params_[0], // TODO: use independent param with whole scan range
      true,
      true,
      true))) {
  }
  return ret;
}

int ObDASLegacyTRMergeIter::init_das_iter_scan_param(const ObTabletID &tablet_id,
                                               const ObDASScanCtDef *ctdef,
                                               ObDASScanRtDef *rtdef,
                                               transaction::ObTxDesc *tx_desc,
                                               transaction::ObTxReadSnapshot *snapshot,
                                               common::ObArenaAllocator &allocator,
                                               ObTableScanParam &scan_param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctdef) || OB_ISNULL(rtdef)) {
    ret = OB_INVALID_ARGUMENT;
  } else {


    scan_param.key_ranges_.set_attr(ObMemAttr("ScanParamKR"));
    scan_param.tx_lock_timeout_ = rtdef->tx_lock_timeout_;
    scan_param.index_id_ = ctdef->ref_table_id_;
    scan_param.is_get_ = false; // scan
    scan_param.is_for_foreign_check_ = false;
    scan_param.timeout_ = rtdef->timeout_ts_;
    scan_param.scan_flag_ = rtdef->scan_flag_;
    scan_param.reserved_cell_count_ = ctdef->access_column_ids_.count();
    scan_param.allocator_ = &rtdef->stmt_allocator_;
    scan_param.scan_allocator_ = &allocator;
    scan_param.sql_mode_ = rtdef->sql_mode_;
    scan_param.frozen_version_ = rtdef->frozen_version_;
    scan_param.output_exprs_ = &(ctdef->pd_expr_spec_.access_exprs_);
    scan_param.calc_exprs_ = &(ctdef->pd_expr_spec_.calc_exprs_);
    scan_param.aggregate_exprs_ = &(ctdef->pd_expr_spec_.pd_storage_aggregate_output_);
    scan_param.table_param_ = &(ctdef->table_param_);
    scan_param.op_ = rtdef->p_pd_expr_op_;
    scan_param.row2exprs_projector_ = rtdef->p_row2exprs_projector_;
    scan_param.schema_version_ = ctdef->schema_version_;
    scan_param.runtime_schema_version_ = rtdef->runtime_schema_version_;
    scan_param.limit_param_ = rtdef->limit_param_;
    scan_param.need_scn_ = rtdef->need_scn_;
    scan_param.pd_storage_flag_ = ctdef->pd_expr_spec_.pd_storage_flag_.pd_flag_;
    scan_param.fb_snapshot_ = rtdef->fb_snapshot_;
    scan_param.tablet_id_ = tablet_id;
    if (!ctdef->pd_expr_spec_.pushdown_filters_.empty()) {
      scan_param.op_filters_ = &ctdef->pd_expr_spec_.pushdown_filters_;
    }
    scan_param.pd_storage_filters_ = rtdef->p_pd_expr_op_->pd_storage_filters_;
    if (OB_NOT_NULL(tx_desc)) {
      scan_param.tx_id_ = tx_desc->get_tx_id();
    } else {
      scan_param.tx_id_.reset();
    }

    if (OB_NOT_NULL(snapshot)) {
      if (OB_FAIL(scan_param.snapshot_.assign(*snapshot))) {
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("null snapshot", K(ret), KP(snapshot));
    }

    if (FAILEDx(scan_param.column_ids_.assign(ctdef->access_column_ids_))) {
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::create_dim_iters()
{
  int ret = OB_SUCCESS;
  if (use_compact_fts_) {
    ObFtsCompactScanReader reader(myself_allocator_);
    common::ObSEArray<common::ObString, 4> docs;
    if (OB_FAIL(reader.init(compact_entry_, snapshot_))) {
    } else if (OB_FAIL(reader.fetch_multi(query_tokens_, docs))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < query_tokens_.count(); ++i) {
        ObFtsCompactDimIter *compact_iter = nullptr;
        if (OB_ISNULL(compact_iter = OB_NEWx(ObFtsCompactDimIter, &myself_allocator_,
                                              myself_allocator_))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(compact_iter->init_from_docs(compact_entry_, docs.at(i)))) {
        } else if (OB_FAIL(dim_iters_.push_back(compact_iter))) {
        }
      }
    }
  } else if (topk_mode_) {
    if (OB_FAIL(init_block_max_iter_param())) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < query_tokens_.count(); ++i) {
      ObTextRetrievalScanIterParam iter_param;
      ObTextRetrievalBlockMaxIter *dim_iter = nullptr;
      if (OB_FAIL(init_dim_iter_param(iter_param, i))) {
      } else if (OB_ISNULL(dim_iter = OB_NEWx(ObTextRetrievalBlockMaxIter, &myself_allocator_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(dim_iter->init(iter_param, block_max_iter_param_, *block_max_scan_params_[i]))) {
      } else if (OB_FAIL(dim_iters_.push_back(dim_iter))) {
      }
    }
  } else if (daat_mode_) {
    for (int64_t i = 0; OB_SUCC(ret) && i < query_tokens_.count(); ++i) {
      ObTextRetrievalScanIterParam iter_param;
      ObTextRetrievalDaaTTokenIter *dim_iter = nullptr;
      if (OB_FAIL(init_dim_iter_param(iter_param, i))) {
      } else if (OB_ISNULL(dim_iter = OB_NEWx(ObTextRetrievalDaaTTokenIter, &myself_allocator_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(dim_iter->init(iter_param))) {
      } else if (OB_FAIL(dim_iters_.push_back(dim_iter))) {
      }
    }
  } else if (taat_mode_) {
    ObTextRetrievalScanIterParam iter_param;
    ObTextRetrievalTokenIter *dim_iter = nullptr;
    if (OB_FAIL(init_dim_iter_param(iter_param, 0))) {
    } else if (OB_ISNULL(dim_iter = OB_NEWx(ObTextRetrievalTokenIter, &myself_allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(dim_iter->init(iter_param))) {
    } else {
      dim_iter_ = dim_iter;
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObDASLegacyTRMergeIter::init_dim_iter_param(ObTextRetrievalScanIterParam &iter_param, const int64_t &idx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ir_ctdef_) || OB_ISNULL(ir_rtdef_) || OB_ISNULL(ir_rtdef_->eval_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const int64_t dim_iter_cnt = taat_mode_ ? 1 : query_tokens_.count();
    iter_param.inv_idx_scan_param_ = inv_scan_params_[idx];
    iter_param.inv_idx_scan_iter_ = static_cast<ObDASScanIter*>(children_[idx]);
    if (ir_ctdef_->need_inv_idx_agg()) {
      iter_param.inv_idx_agg_param_ = inv_agg_params_[idx];
      iter_param.inv_idx_agg_iter_ = static_cast<ObDASScanIter*>(children_[idx + dim_iter_cnt]);
      if (OB_UNLIKELY(1 != ir_ctdef_->get_inv_idx_agg_ctdef()
          ->pd_expr_spec_.pd_storage_aggregate_output_.count())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        iter_param.inv_idx_agg_expr_ = ir_ctdef_->get_inv_idx_agg_ctdef()
            ->pd_expr_spec_.pd_storage_aggregate_output_.at(0);
      }
    }
    iter_param.allocator_ = &myself_allocator_;
    iter_param.mem_context_ = mem_context_;
    iter_param.eval_ctx_ = ir_rtdef_->eval_ctx_;
    iter_param.relevance_expr_ = ir_ctdef_->relevance_expr_;
    iter_param.inv_scan_doc_length_col_ = ir_ctdef_->inv_scan_doc_length_col_;
    iter_param.inv_scan_domain_id_col_ = ir_ctdef_->inv_scan_domain_id_col_;
    iter_param.inv_idx_agg_cache_mode_ = function_lookup_mode_ && !taat_mode_;
  }
  return ret;
}

int ObDASLegacyTRMergeIter::create_sparse_retrieval_iter()
{
  int ret = OB_SUCCESS;
  sr_iter_param_.dim_weights_ = dim_weights_.count() >= 1 ? &dim_weights_ : nullptr;
  sr_iter_param_.limit_param_ = &ir_rtdef_->get_inv_idx_scan_rtdef()->limit_param_;
  sr_iter_param_.eval_ctx_ = ir_rtdef_->eval_ctx_;
  sr_iter_param_.id_proj_expr_ = ir_ctdef_->inv_scan_domain_id_col_;
  sr_iter_param_.relevance_expr_ = ir_ctdef_->relevance_expr_;
  sr_iter_param_.relevance_proj_expr_ = ir_ctdef_->relevance_proj_col_;
  sr_iter_param_.filter_expr_ = ir_ctdef_->match_filter_;
  sr_iter_param_.topk_limit_ = topk_limit_;
  sr_iter_param_.skip_bm25_estimation_ = use_compact_fts_;
  if (OB_NOT_NULL(ir_ctdef_->field_boost_expr_)) {
    ObDatum *boost_datum = nullptr;
    if (OB_FAIL(ir_ctdef_->field_boost_expr_->eval(*ir_rtdef_->eval_ctx_, boost_datum))) {
    } else if (OB_ISNULL(boost_datum) || OB_UNLIKELY(boost_datum->is_null())) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "null field boost is");
    } else {
      sr_iter_param_.field_boost_ = boost_datum->get_double();
      if (sr_iter_param_.field_boost_ <= 0.0) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "field boost < 0 is");
      } else if (query_tokens_.count() == 0) {
        // do nothing
      } else if (OB_ISNULL(sr_iter_param_.dim_weights_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < dim_weights_.count(); ++i) {
          if (sr_iter_param_.dim_weights_->at(i) <= 0.0) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "token weight < 0 is");
          }
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (use_compact_fts_) {
    ObFtsCompactMergeIter *compact_iter = nullptr;
    if (OB_ISNULL(compact_iter = OB_NEWx(ObFtsCompactMergeIter, &myself_allocator_,
                                          myself_allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(compact_iter->init(&sr_iter_param_, &dim_iters_,
                                          ir_rtdef_->minimum_should_match_))) {
    } else {
      compact_iter->set_skip_projection(skip_projection_);
      sparse_retrieval_iter_ = compact_iter;
    }
  } else if (topk_mode_) {
    ObTextDaaTParam iter_param;
    ObTextBMWIter *bmw_iter = nullptr;
    if (OB_FAIL(init_daat_iter_param(iter_param))) {
    } else if (OB_ISNULL(bmw_iter = OB_NEWx(ObTextBMWIter, &myself_allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(bmw_iter->init(iter_param))) {
    } else {
      sparse_retrieval_iter_ = bmw_iter;
    }
  } else if (daat_mode_) {
    ObTextDaaTParam iter_param;
    ObTextDaaTIter *daat_iter = nullptr;
    if (OB_FAIL(init_daat_iter_param(iter_param))) {
    } else if (OB_ISNULL(daat_iter = OB_NEWx(ObTextDaaTIter, &myself_allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(daat_iter->init(iter_param))) {
    } else {
      sparse_retrieval_iter_ = daat_iter;
    }
  } else if (taat_mode_) {
    ObTextTaaTParam iter_param;
    ObTextTaaTIter *taat_iter = nullptr;
    if (OB_FAIL(init_taat_iter_param(iter_param))) {
    } else if (OB_ISNULL(taat_iter = OB_NEWx(ObTextTaaTIter, &myself_allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(taat_iter->init(iter_param))) {
    } else {
      sparse_retrieval_iter_ = taat_iter;
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  if (OB_SUCC(ret) && function_lookup_mode_) {
    ObSRLookupIter *lookup_iter = nullptr;
    if (OB_UNLIKELY(topk_mode_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (daat_mode_) {
      if (OB_ISNULL(lookup_iter = OB_NEWx(ObSRSortedLookupIter, &myself_allocator_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    } else if (taat_mode_) {
      if (OB_ISNULL(lookup_iter = OB_NEWx(ObSRHashLookupIter, &myself_allocator_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
    }
    if (FAILEDx(lookup_iter->init(sr_iter_param_, *sparse_retrieval_iter_,
                                  myself_allocator_, sr_iter_param_.max_batch_size_))) {
    } else {
      sparse_retrieval_iter_ = lookup_iter;
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::init_daat_iter_param(ObTextDaaTParam &iter_param)
{
  int ret = OB_SUCCESS;
  iter_param.dim_iters_ = &dim_iters_;
  iter_param.base_param_ = &sr_iter_param_;
  iter_param.allocator_ = &myself_allocator_;
  iter_param.mode_flag_ = ir_ctdef_->mode_flag_;
  iter_param.function_lookup_mode_ = function_lookup_mode_;
  iter_param.minimum_should_match_ = ir_rtdef_->minimum_should_match_;
  if (use_compact_fts_) {
    iter_param.bm25_param_est_ctx_.estimated_total_doc_cnt_ = 1;
    iter_param.bm25_param_est_ctx_.need_est_avg_doc_token_cnt_ = false;
    iter_param.bm25_param_est_ctx_.can_est_by_sum_skip_index_ = false;
    iter_param.bm25_param_est_ctx_.doc_length_est_param_ = &doc_length_est_param_;
  } else if (ir_ctdef_->need_calc_relevance()) {
    iter_param.bm25_param_est_ctx_.total_doc_cnt_expr_
        = ir_ctdef_->get_doc_agg_ctdef()->pd_expr_spec_.pd_storage_aggregate_output_.at(0);
    iter_param.bm25_param_est_ctx_.estimated_total_doc_cnt_ = ir_ctdef_->estimated_total_doc_cnt_;
    iter_param.bm25_param_est_ctx_.need_est_avg_doc_token_cnt_ = ir_ctdef_->need_avg_doc_len_est();
    iter_param.bm25_param_est_ctx_.can_est_by_sum_skip_index_ = ir_ctdef_->avg_doc_len_est_spec_.can_est_by_sum_skip_index_;
    iter_param.bm25_param_est_ctx_.avg_doc_token_cnt_expr_ = ir_ctdef_->avg_doc_token_cnt_expr_;
    iter_param.bm25_param_est_ctx_.doc_length_est_param_ = &doc_length_est_param_;
    if (query_tokens_.count() == 0) {
      // do nothing
    } else if (OB_ISNULL(iter_param.bm25_param_est_ctx_.total_doc_cnt_expr_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(init_doc_length_est_param())) {
    } else if (!ir_ctdef_->need_estimate_total_doc_cnt()) {
      if (OB_UNLIKELY(!static_cast<sql::ObStoragePushdownFlag>(total_doc_cnt_scan_param_->pd_storage_flag_).is_aggregate_pushdown())) {
        ret = OB_NOT_IMPLEMENT;
        LOG_ERROR("aggregate without pushdown not implemented", K(ret));
      } else {
        iter_param.bm25_param_est_ctx_.total_doc_cnt_iter_ = static_cast<sql::ObDASScanIter*>(children_[children_cnt_ - 1]);
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (BOOLEAN_MODE == ir_ctdef_->mode_flag_) {
      ObSRDaaTBooleanRelevanceCollector *boolean_relevance_collector = nullptr;
      if (OB_ISNULL(boolean_relevance_collector = OB_NEWx(ObSRDaaTBooleanRelevanceCollector, &myself_allocator_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(boolean_relevance_collector->init(&myself_allocator_, dim_iters_.count(), boolean_compute_node_))) {
      } else {
        iter_param.relevance_collector_ = boolean_relevance_collector;
      }
    } else {
      int64_t should_match = ir_rtdef_->minimum_should_match_;
      ObSRDaaTInnerProductRelevanceCollector *inner_product_relevance_collector = nullptr;
      if (OB_ISNULL(inner_product_relevance_collector = OB_NEWx(ObSRDaaTInnerProductRelevanceCollector, &myself_allocator_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(inner_product_relevance_collector->init(should_match))) {
      } else {
        iter_param.relevance_collector_ = inner_product_relevance_collector;
      }
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::init_taat_iter_param(ObTextTaaTParam &iter_param)
{
  int ret = OB_SUCCESS;
  iter_param.query_tokens_ = &query_tokens_;
  iter_param.base_param_ = &sr_iter_param_;
  iter_param.allocator_ = &myself_allocator_;
  iter_param.mode_flag_ = ir_ctdef_->mode_flag_;
  iter_param.function_lookup_mode_ = function_lookup_mode_;
  if (ir_ctdef_->need_calc_relevance()) {
    iter_param.bm25_param_est_ctx_.total_doc_cnt_expr_
        = ir_ctdef_->get_doc_agg_ctdef()->pd_expr_spec_.pd_storage_aggregate_output_.at(0);
    iter_param.bm25_param_est_ctx_.estimated_total_doc_cnt_ = ir_ctdef_->estimated_total_doc_cnt_;
    iter_param.bm25_param_est_ctx_.need_est_avg_doc_token_cnt_ = ir_ctdef_->need_avg_doc_len_est();
    iter_param.bm25_param_est_ctx_.can_est_by_sum_skip_index_ = ir_ctdef_->avg_doc_len_est_spec_.can_est_by_sum_skip_index_;
    iter_param.bm25_param_est_ctx_.avg_doc_token_cnt_expr_ = ir_ctdef_->avg_doc_token_cnt_expr_;
    iter_param.bm25_param_est_ctx_.doc_length_est_param_ = &doc_length_est_param_;
  }
  if (OB_ISNULL(iter_param.dim_iter_ = dim_iter_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!ir_ctdef_->need_calc_relevance()) {
    // Membership queries need neither corpus statistics nor BM25.
  } else if (OB_ISNULL(iter_param.bm25_param_est_ctx_.total_doc_cnt_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(init_doc_length_est_param())) {
  } else if (!ir_ctdef_->need_estimate_total_doc_cnt()) {
    if (OB_UNLIKELY(!static_cast<sql::ObStoragePushdownFlag>(total_doc_cnt_scan_param_->pd_storage_flag_).is_aggregate_pushdown())) {
      ret = OB_NOT_IMPLEMENT;
      LOG_ERROR("aggregate without pushdown not implemented", K(ret));
    } else {
      iter_param.bm25_param_est_ctx_.total_doc_cnt_iter_ = static_cast<sql::ObDASScanIter*>(children_[children_cnt_ - 1]);
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::set_children_iter_rangekey()
{
  int ret = OB_SUCCESS;
  check_rangekey_inited_ = true;
  if (OB_UNLIKELY(function_lookup_mode_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(0 == query_tokens_.count())) {
    // do nothing
  } else if (OB_UNLIKELY(inv_scan_params_.empty()
      || (ir_ctdef_->need_inv_idx_agg() && inv_agg_params_.empty()))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ExprFixedArray *exprs = &(ir_ctdef_->get_inv_idx_scan_ctdef()->pd_expr_spec_.access_exprs_);
    int64 group_id = 0;
    for (int64_t i = 0; i < exprs->count(); ++i) {
      if (T_PSEUDO_GROUP_ID == exprs->at(i)->type_) {
        group_id = exprs->at(i)->locate_expr_datum(*eval_ctx_).get_int();
      }
    }
    int64_t group_idx = ObNewRange::get_group_idx(group_id);
    ObNewRange inv_idx_scan_range;
    const int64_t dim_iter_cnt = taat_mode_ ? 1 : query_tokens_.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iter_cnt; ++i) {
      if (OB_FAIL(gen_inv_idx_scan_default_range(query_tokens_[i], inv_idx_scan_range))) {
      } else if (ir_ctdef_->need_inv_idx_agg()
          && OB_FAIL(inv_agg_params_[i]->key_ranges_.push_back(inv_idx_scan_range))) {
      } else if (FALSE_IT(inv_idx_scan_range.group_idx_ = group_idx)) {
      } else if (OB_FAIL(inv_scan_params_[i]->key_ranges_.push_back(inv_idx_scan_range))) {
      } else if (topk_mode_ && ir_ctdef_->need_block_max_scan()
          && OB_FAIL(block_max_scan_params_[i]->key_ranges_.push_back(inv_idx_scan_range))) {
      }
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::set_children_iter_rangekey(const common::ObIArray<std::pair<ObDocIdExt, int>> &virtual_rangekeys, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  check_rangekey_inited_ = true;
  if (OB_UNLIKELY(!function_lookup_mode_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (nullptr == sparse_retrieval_iter_) {
    if (OB_FAIL(init_das_iter_scan_params())) {
    } else if (OB_FAIL(create_dim_iters())) {
    } else if (OB_FAIL(create_sparse_retrieval_iter())) {
    }
  }
  if (OB_FAIL(ret) || 0 == query_tokens_.count()) {
  } else if (OB_UNLIKELY(inv_scan_params_.empty()
      || (ir_ctdef_->need_inv_idx_agg() && inv_agg_params_.empty()))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ExprFixedArray *exprs = &(ir_ctdef_->get_inv_idx_scan_ctdef()->pd_expr_spec_.access_exprs_);
    int64 group_id = 0;
    for (int64_t i = 0; i < exprs->count(); ++i) {
      if (T_PSEUDO_GROUP_ID == exprs->at(i)->type_) {
        group_id = exprs->at(i)->locate_expr_datum(*eval_ctx_).get_int();
      }
    }
    int64_t group_idx = ObNewRange::get_group_idx(group_id);
    ObNewRange inv_idx_scan_range;
    const int64_t dim_iter_cnt = taat_mode_ ? 1 : query_tokens_.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iter_cnt; ++i) {
      for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; ++j) {
        if (OB_FAIL(gen_inv_idx_scan_one_range(query_tokens_[i],
                                               virtual_rangekeys.at(j).first,
                                               inv_idx_scan_range))) {
        } else if (FALSE_IT(inv_idx_scan_range.group_idx_ = group_idx)) {
        } else if (OB_FAIL(inv_scan_params_[i]->key_ranges_.push_back(inv_idx_scan_range))) {
        }
      }

      if (!ir_ctdef_->need_inv_idx_agg()) {
      } else if (OB_FAIL(gen_inv_idx_scan_default_range(query_tokens_[i], inv_idx_scan_range))) {
      } else if (OB_FAIL(inv_agg_params_[i]->key_ranges_.push_back(inv_idx_scan_range))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(sparse_retrieval_iter_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(static_cast<ObSRLookupIter *>(sparse_retrieval_iter_)
      ->set_hints(virtual_rangekeys, batch_size))) {
  }
  return ret;
}

int ObDASLegacyTRMergeIter::gen_inv_idx_scan_default_range(const ObString &query_token, ObNewRange &scan_range)
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  ObObj *obj_ptr = nullptr;
  common::ObArenaAllocator &ctx_alloc = mem_context_->get_arena_allocator();
  constexpr int64_t obj_cnt = INV_IDX_ROWKEY_COL_CNT * 2;
  ObObj tmp_obj;
  tmp_obj.set_string(ObVarcharType, query_token);
  // We need to ensure collation type / level between query text and token column is compatible
  tmp_obj.set_meta_type(ir_ctdef_->search_text_->obj_meta_);

  if (OB_ISNULL(buf = ctx_alloc.alloc(sizeof(ObObj) * obj_cnt))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(obj_ptr = new (buf) ObObj[obj_cnt])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ob_write_obj(ctx_alloc, tmp_obj, obj_ptr[0]))) {
  } else if (OB_FAIL(ob_write_obj(ctx_alloc, tmp_obj, obj_ptr[2]))) {
  } else {
    obj_ptr[1].set_min_value();
    obj_ptr[3].set_max_value();
    ObRowkey start_key(obj_ptr, INV_IDX_ROWKEY_COL_CNT);
    ObRowkey end_key(&obj_ptr[2], INV_IDX_ROWKEY_COL_CNT);
    common::ObTableID inv_table_id = ir_ctdef_->get_inv_idx_scan_ctdef()->ref_table_id_;
    scan_range.table_id_ = inv_table_id;
    scan_range.start_key_.assign(obj_ptr, INV_IDX_ROWKEY_COL_CNT);
    scan_range.end_key_.assign(&obj_ptr[2], INV_IDX_ROWKEY_COL_CNT);
    scan_range.border_flag_.set_inclusive_start();
    scan_range.border_flag_.set_inclusive_end();
  }
  return ret;
}

int ObDASLegacyTRMergeIter::gen_inv_idx_scan_one_range(const ObString &query_token, const ObDocIdExt &doc_id, ObNewRange &scan_range)
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  ObObj *obj_ptr = nullptr;
  common::ObArenaAllocator &ctx_alloc = mem_context_->get_arena_allocator();
  constexpr int64_t obj_cnt = INV_IDX_ROWKEY_COL_CNT;
  ObObj tmp_obj;
  tmp_obj.set_string(ObVarcharType, query_token);
  // We need to ensure collation type / level between query text and token column is compatible
  tmp_obj.set_meta_type(ir_ctdef_->search_text_->obj_meta_);

  if (OB_ISNULL(buf = ctx_alloc.alloc(sizeof(ObObj) * obj_cnt))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(obj_ptr = new (buf) ObObj[obj_cnt])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ob_write_obj(ctx_alloc, tmp_obj, obj_ptr[0]))) {
  } else if (OB_FAIL(doc_id.get_datum().to_obj(obj_ptr[1], ir_ctdef_->inv_scan_domain_id_col_->obj_meta_))) {
  } else {
    ObRowkey row_key(obj_ptr, obj_cnt);
    common::ObTableID inv_table_id = ir_ctdef_->get_inv_idx_scan_ctdef()->ref_table_id_;
    if (OB_FAIL(scan_range.build_range(inv_table_id, row_key))) {
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::do_table_scan()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (use_compact_fts_) {
    if (OB_FAIL(create_dim_iters())) {
    } else if (OB_FAIL(create_sparse_retrieval_iter())) {
    } else {
      check_rangekey_inited_ = true;
    }
  } else {
    if (function_lookup_mode_) {
      // skip the following, which should have been done
    } else if (OB_FAIL(init_das_iter_scan_params())) {
    } else if (OB_FAIL(create_dim_iters())) {
    } else if (OB_FAIL(create_sparse_retrieval_iter())) {
    } else if (OB_FAIL(set_children_iter_rangekey())) {
    }
    if (OB_FAIL(ret)) {
    } else if (OB_UNLIKELY(!check_rangekey_inited_)) {
      ret = OB_ERR_UNEXPECTED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < children_cnt_; ++i) {
      if (OB_FAIL(children_[i]->do_table_scan())) {
      }
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::reuse_das_iter_scan_param(const ObTabletID &tablet_id,
                                                ObTableScanParam &scan_param)
{
  int ret = OB_SUCCESS;
  const ObTabletID &old_tablet_id = scan_param.tablet_id_;
  scan_param.need_switch_param_ = scan_param.need_switch_param_
      || (old_tablet_id.is_valid() && old_tablet_id != tablet_id);
  if (!scan_param.key_ranges_.empty()) {
    scan_param.key_ranges_.reuse();
  }
  scan_param.tablet_id_ = tablet_id;
  return ret;
}

int ObDASLegacyTRMergeIter::inner_reuse()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sparse_retrieval_iter_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (query_tokens_.count() > 0) {
    if (OB_NOT_NULL(total_doc_cnt_scan_param_)) {
      if (OB_FAIL(reuse_das_iter_scan_param(total_doc_cnt_tablet_id_, *total_doc_cnt_scan_param_))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < inv_scan_params_.count(); ++i) {
      if (OB_FAIL(reuse_das_iter_scan_param(inv_idx_tablet_id_, *inv_scan_params_[i]))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < inv_agg_params_.count(); ++i) {
      if (OB_FAIL(reuse_das_iter_scan_param(inv_idx_tablet_id_, *inv_agg_params_[i]))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < block_max_scan_params_.count(); ++i) {
      if (OB_FAIL(reuse_das_iter_scan_param(inv_idx_tablet_id_, *block_max_scan_params_[i]))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < children_cnt_; ++i) {
      if (OB_FAIL(children_[i]->reuse())) {
      }
    }
  }

  if (OB_SUCC(ret)) {
    sparse_retrieval_iter_->reuse(inv_idx_tablet_switched_);
    inv_idx_tablet_switched_ = false;
  }

  if (OB_NOT_NULL(mem_context_)) {
    mem_context_->reset_remain_one_page();
  }
  check_rangekey_inited_ = false;
  return ret;
}

int ObDASLegacyTRMergeIter::rescan()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sparse_retrieval_iter_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (use_compact_fts_) {
    sparse_retrieval_iter_->reuse(false);
  } else if (!function_lookup_mode_ && OB_FAIL(set_children_iter_rangekey())) {
  } else if (OB_UNLIKELY(!check_rangekey_inited_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < children_cnt_; ++i) {
      // TODO: update scan param
      if (OB_FAIL(children_[i]->rescan())) {
      }
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::inner_release()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(total_doc_cnt_scan_param_)) {
    total_doc_cnt_scan_param_->destroy_schema_guard();
    total_doc_cnt_scan_param_->snapshot_.reset();
    total_doc_cnt_scan_param_->destroy();
    total_doc_cnt_scan_param_ = nullptr;
  }

  for (int64_t i = 0; i < inv_scan_params_.count(); ++i) {
    if (OB_NOT_NULL(inv_scan_params_[i])) {
      inv_scan_params_[i]->destroy_schema_guard();
      inv_scan_params_[i]->snapshot_.reset();
      inv_scan_params_[i]->destroy();
    }
  }
  inv_scan_params_.reset();

  for (int64_t i = 0; i < inv_agg_params_.count(); ++i) {
    if (OB_NOT_NULL(inv_agg_params_[i])) {
      inv_agg_params_[i]->destroy_schema_guard();
      inv_agg_params_[i]->snapshot_.reset();
      inv_agg_params_[i]->destroy();
    }
  }
  inv_agg_params_.reset();

  for (int64_t i = 0; i < block_max_scan_params_.count(); ++i) {
    if (OB_NOT_NULL(block_max_scan_params_[i])) {
      block_max_scan_params_[i]->destroy_schema_guard();
      block_max_scan_params_[i]->snapshot_.reset();
      block_max_scan_params_[i]->destroy();
    }
  }
  block_max_scan_params_.reset();

  if (OB_NOT_NULL(boolean_compute_node_)) {
    boolean_compute_node_->release();
    boolean_compute_node_ = nullptr;
  }

  if (OB_NOT_NULL(sparse_retrieval_iter_)) {
    sparse_retrieval_iter_->reset();
    sparse_retrieval_iter_ = nullptr;
  }
  dim_iters_.reset();
  dim_iter_ = nullptr;
  if (nullptr != mem_context_) {
    mem_context_->reset_remain_one_page();
    DESTROY_CONTEXT(mem_context_);
    mem_context_ = nullptr;
  }
  query_tokens_.reset();
  dim_weights_.reset();
  myself_allocator_.reset();
  ir_ctdef_ = nullptr;
  ir_rtdef_ = nullptr;
  tx_desc_ = nullptr;
  snapshot_ = nullptr;
  topk_limit_ = 0;
  inv_idx_tablet_switched_ = false;
  is_inited_ = false;
  return ret;
}

int ObDASLegacyTRMergeIter::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sparse_retrieval_iter_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = sparse_retrieval_iter_->get_next_row();
  }
  return ret;
}

int ObDASLegacyTRMergeIter::inner_get_next_rows(int64_t &count, int64_t capacity)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sparse_retrieval_iter_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(0 == capacity)) {
    count = 0;
  } else {
    ret = sparse_retrieval_iter_->get_next_rows(capacity, count);
  }
  return ret;
}

static int get_query_tokens_by_compacting_repeated_token(ObString &query_str,
                                                         const ObCollationType &cs_type,
                                                         ObArray<ObString> &query_tokens,
                                                         ObArray<double> &boost_values,
                                                         common::ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  char split_token_tag = ' ';
  char split_boost_tag = '^';
  hash::ObHashMap<ObString, double> token_map;
  const int64_t ft_word_bkt_cnt = MAX(query_str.length() / 10, 2);
  if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  }
  while (!query_str.empty() && OB_SUCC(ret)) {
    ObString token_str = query_str.split_on(split_token_tag);
    query_str = query_str.trim();
    if (token_str.empty()) {
      token_str = query_str;
      query_str.reset();
    }
    ObString token_key = token_str.split_on(split_boost_tag);
    double boost_value = 1.0;
    if (token_key.empty()) {
      token_key = token_str;
      token_str.reset();
    } else {
      char *boost_str = static_cast<char *>(alloc.alloc(token_str.length() + 1));
      if (OB_ISNULL(boost_str)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        memcpy(boost_str, token_str.ptr(), token_str.length());
        boost_str[token_str.length()] = '\0';
        char *end_ptr = nullptr;
        boost_value = strtod(boost_str, &end_ptr);
        if (end_ptr != boost_str + token_str.length() || boost_value <= 0.0) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "invalid field boost");
        }
      }
    }
    double cur_boost = 0;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(token_map.get_refactored(token_key, cur_boost))) {
      if (OB_HASH_NOT_EXIST != ret) {
      } else if (OB_FAIL(token_map.set_refactored(token_key, boost_value, 1/*overwrite*/))) {
      }
    } else if (OB_FAIL(token_map.set_refactored(token_key, boost_value + cur_boost, 1/*overwrite*/))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(query_tokens.reserve(token_map.size()))) {
  } else if (OB_FAIL(boost_values.reserve(token_map.size()))) {
  }
  for (hash::ObHashMap<ObString, double>::const_iterator iter = token_map.begin();
      OB_SUCC(ret) && iter != token_map.end();
      ++iter) {
    const ObString &token = iter->first;
    ObString token_string;
    if (OB_FAIL(common::ObCharset::charset_convert(alloc, token, ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI, cs_type, token_string, common::ObCharset::CONVERT_FLAG::COPY_STRING_ON_SAME_CHARSET))) {
    } else if (OB_FAIL(query_tokens.push_back(token_string))) {
    } else if (OB_FAIL(boost_values.push_back(iter->second))) {
    }
  }
  return ret;
}

static int get_query_tokens_directly(ObString &query_str,
                                     const ObCollationType &cs_type,
                                     ObArray<ObString> &query_tokens,
                                     ObArray<double> &boost_values,
                                     common::ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  char split_token_tag = ' ';
  char split_boost_tag = '^';
  while (!query_str.empty() && OB_SUCC(ret)) {
    ObString token_str = query_str.split_on(split_token_tag);
    query_str = query_str.trim();
    if (token_str.empty()) {
      token_str = query_str;
      query_str.reset();
    }
    ObString token_key = token_str.split_on(split_boost_tag);
    double boost_value = 1.0;
    if (token_key.empty()) {
      token_key = token_str;
      token_str.reset();
    } else {
      char *boost_str = static_cast<char *>(alloc.alloc(token_str.length() + 1));
      if (OB_ISNULL(boost_str)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        memcpy(boost_str, token_str.ptr(), token_str.length());
        boost_str[token_str.length()] = '\0';
        char *end_ptr = nullptr;
        boost_value = strtod(boost_str, &end_ptr);
        if (end_ptr != boost_str + token_str.length() || boost_value <= 0.0) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "invalid field boost");
        }
      }
    }
    double cur_boost = 0;
    if (OB_FAIL(ret)) {
    } else {
      ObString token_string;
      if (OB_FAIL(common::ObCharset::charset_convert(alloc, token_key, ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI, cs_type, token_string, common::ObCharset::CONVERT_FLAG::COPY_STRING_ON_SAME_CHARSET))) {
      } else if (OB_FAIL(query_tokens.push_back(token_string))) {
      } else if (OB_FAIL(boost_values.push_back(boost_value))) {
      }
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::build_query_tokens(const ObDASIRScanCtDef *ir_ctdef,
                                         ObDASIRScanRtDef *ir_rtdef,
                                         common::ObIAllocator &alloc,
                                         ObArray<ObString> &query_tokens,
                                         ObArray<double> &boost_values,
                                         ObFtsEvalNode *&root_node,
                                         bool &has_duplicate_boolean_tokens)
{
  int ret = OB_SUCCESS;
  has_duplicate_boolean_tokens = false;
  ObExpr *search_text = ir_ctdef->search_text_;
  ObEvalCtx *eval_ctx = ir_rtdef->eval_ctx_;
  ObDatum *search_text_datum = nullptr;
  if (OB_ISNULL(search_text) || OB_ISNULL(eval_ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (query_tokens.count() != 0 || boost_values.count() != 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(search_text->eval(*eval_ctx, search_text_datum))) {
  } else if (0 == search_text_datum->len_) {
    // empty query text
  } else if (OB_NOT_NULL(ir_ctdef->field_boost_expr_)) {
    const ObString &search_text_string = search_text_datum->get_string();
    const ObCollationType &cs_type = search_text->datum_meta_.cs_type_;
    const ObCollationType dst_type = ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI;

    ObString str_dest;
    if (cs_type != dst_type) {
      ObString tmp_out;
      if (OB_FAIL(ObCharset::tolower(cs_type, search_text_string, tmp_out, alloc))) {
      } else if (OB_FAIL(common::ObCharset::charset_convert(alloc, tmp_out, cs_type, dst_type, str_dest))) {
      }
    } else if (OB_FAIL(ObCharset::tolower(cs_type, search_text_string, str_dest, alloc))){
    }
    if (OB_FAIL(ret)) {
    } else {
      const bool compact_repeated_token = ir_rtdef->minimum_should_match_ <= 1;
      ObString query_str = str_dest.trim();
      if (compact_repeated_token && OB_FAIL(get_query_tokens_by_compacting_repeated_token(query_str, cs_type, query_tokens, boost_values, alloc))) {
      } else if (!compact_repeated_token && OB_FAIL(get_query_tokens_directly(query_str, cs_type, query_tokens, boost_values, alloc))) {
      }
    }
  } else if (BOOLEAN_MODE == ir_ctdef->mode_flag_) {
    const ObString &search_text_string = search_text_datum->get_string();
    const ObCollationType &cs_type = search_text->datum_meta_.cs_type_;
    const ObCollationType dst_type = ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI;

    ObString str_dest;
    if (cs_type != dst_type) {
      ObString tmp_out;
      if (OB_FAIL(ObCharset::tolower(cs_type, search_text_string, tmp_out, alloc))) {
      } else if (OB_FAIL(common::ObCharset::charset_convert(alloc, tmp_out, cs_type, dst_type, str_dest))) {
      }
    } else if (OB_FAIL(ObCharset::tolower(cs_type, search_text_string, str_dest, alloc))){
    }

    void *buf = nullptr;
    FtsParserResult *fts_parser;
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(buf = (&alloc)->alloc(sizeof(FtsParserResult)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      fts_parser = static_cast<FtsParserResult *>(buf);
    }

    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(buf = (&alloc)->alloc(str_dest.length() + 1))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMSET(buf, 0, str_dest.length() + 1);
      MEMCPY(buf, str_dest.ptr(), str_dest.length());
    }

    if (OB_FAIL(ret)) {
    } else if (FALSE_IT(fts_parse_docment(static_cast<char *>(buf), str_dest.length(), &alloc, fts_parser))) {
    } else if (FTS_OK != fts_parser->ret_) {
      if (FTS_ERROR_MEMORY == fts_parser->ret_) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (FTS_ERROR_SYNTAX == fts_parser->ret_) {
        ret = OB_ERR_PARSER_SYNTAX;
      } else if (FTS_ERROR_OTHER == fts_parser->ret_) {
        ret = OB_ERR_UNEXPECTED;
      }
    } else if (OB_ISNULL(fts_parser->root_)) {
      // do nothing
    } else {
      FtsNode *node = fts_parser->root_;
      ObFtsEvalNode *parant_node =nullptr;
      hash::ObHashMap<ObString, int32_t> tokens_map;
      const int64_t ft_word_bkt_cnt = MAX(search_text_string.length() / 10, 2);
      if (OB_FAIL(tokens_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
      } else if (OB_FAIL(ObFtsEvalNode::fts_boolean_node_create(parant_node, node, cs_type, alloc, query_tokens, tokens_map, has_duplicate_boolean_tokens))) {
      } else {
        root_node = parant_node;
      }
    }
  } else {
    // TODO: FTParseHelper currently does not support deduplicate tokens
    //       We should abstract such universal analyse functors into utility structs
    const ObString &search_text_string = search_text_datum->get_string();
    const ObString &parser_name = ir_ctdef->get_inv_idx_scan_ctdef()->table_param_.get_parser_name();
    const ObString &parser_properties = ir_ctdef->get_inv_idx_scan_ctdef()->table_param_.get_parser_property();

    const ObObjMeta &meta = search_text->obj_meta_;
    int64_t doc_length = 0;
    storage::ObFTParseHelper tokenize_helper;
    common::ObSEArray<ObFTWord, 16> tokens;
    storage::ObFTWordMap token_map;
    const int64_t ft_word_bkt_cnt = MAX(search_text_string.length() / 10, 2);
    if (OB_FAIL(tokenize_helper.init(&alloc, parser_name, parser_properties))) {
    } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
    } else if (OB_FAIL(tokenize_helper.segment(
                           meta,
                           search_text_string.ptr(),
                           search_text_string.length(),
                           doc_length,
                           token_map))) {
    } else {
      for (storage::ObFTWordMap::const_iterator iter = token_map.begin();
          OB_SUCC(ret) && iter != token_map.end();
          ++iter) {
        const ObFTWord &token = iter->first;
        ObString token_string;
        if (OB_FAIL(ob_write_string(alloc, token.get_word().get_string(), token_string))) {
        } else if (OB_FAIL(query_tokens.push_back(token_string))) {
        }
      }
    }
  }
  return ret;
}

int ObDASLegacyTRMergeIter::init_topk_limit()
{
  int ret = OB_SUCCESS;
  topk_limit_ = 0;
  ObExpr *topk_limit_expr = ir_ctdef_->topk_limit_expr_;
  ObExpr *topk_offset_expr = ir_ctdef_->topk_offset_expr_;
  if (OB_UNLIKELY(!topk_mode_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int64_t limit = 0;
    int64_t offset = 0;
    ObDatum *topk_limit_datum = nullptr;
    ObDatum *topk_offset_datum = nullptr;
    if (OB_FAIL(topk_limit_expr->eval(*ir_rtdef_->eval_ctx_, topk_limit_datum))) {
    } else if (nullptr != topk_offset_expr && OB_FAIL(topk_offset_expr->eval(*ir_rtdef_->eval_ctx_, topk_offset_datum))) {
    } else {
      limit = (topk_limit_datum->is_null() || topk_limit_datum->get_int() < 0) ? 0 : topk_limit_datum->get_int();
      if (nullptr != topk_offset_datum) {
        offset = (topk_offset_datum->is_null() || topk_offset_datum->get_int() < 0) ? 0 : topk_offset_datum->get_int();
      }
      topk_limit_ = limit + offset;
    }
  }
  return ret;
}

namespace
{

class ObDASLegacyTextRetrievalEngine final
    : public ObIDASTextRetrievalEngine
{
public:
  explicit ObDASLegacyTextRetrievalEngine(common::ObIAllocator &allocator)
    : allocator_(&allocator), iter_(nullptr)
  {}
  ~ObDASLegacyTextRetrievalEngine() override = default;

  int init(ObDASTRMergeIterParam &param) override
  {
    int ret = OB_SUCCESS;
    void *buffer = nullptr;
    if (OB_NOT_NULL(iter_)) {
      ret = OB_INIT_TWICE;
    } else if (OB_ISNULL(buffer = allocator_->alloc(
        sizeof(ObDASLegacyTRMergeIter)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      iter_ = new (buffer) ObDASLegacyTRMergeIter();
      if (OB_FAIL(iter_->init(param))) {
      }
    }
    return ret;
  }

  int bind_source_tree(
      ObDASIter **children,
      const uint32_t child_count) override
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(iter_)
        || (0 == child_count && nullptr != children)
        || (child_count > 0 && nullptr == children)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      iter_->get_children() = children;
      iter_->set_children_cnt(child_count);
    }
    return ret;
  }

  int set_related_tablet_ids(
      const ObDASFTSTabletID &related_tablet_ids) override
  {
    return OB_ISNULL(iter_)
        ? OB_NOT_INIT
        : iter_->set_related_tablet_ids(related_tablet_ids);
  }

  int do_table_scan() override
  {
    return OB_ISNULL(iter_) ? OB_NOT_INIT : iter_->do_table_scan();
  }

  int reuse() override
  {
    return OB_ISNULL(iter_) ? OB_NOT_INIT : iter_->reuse();
  }

  int rescan() override
  {
    return OB_ISNULL(iter_) ? OB_NOT_INIT : iter_->rescan();
  }

  int get_next_row() override
  {
    return OB_ISNULL(iter_) ? OB_NOT_INIT : iter_->get_next_row();
  }

  int get_next_rows(int64_t &count, const int64_t capacity) override
  {
    count = 0;
    return OB_ISNULL(iter_)
        ? OB_NOT_INIT : iter_->get_next_rows(count, capacity);
  }

  int set_lookup_keys(
      const common::ObIArray<std::pair<ObDocIdExt, int>> &virtual_rangekeys,
      const int64_t batch_size) override
  {
    return OB_ISNULL(iter_)
        ? OB_NOT_INIT
        : iter_->set_children_iter_rangekey(
            virtual_rangekeys, batch_size);
  }

  bool is_taat_mode() const override
  {
    return OB_NOT_NULL(iter_) && iter_->is_taat_mode();
  }

  int get_query_max_score(double &score) override
  {
    score = 0.0;
    return OB_ISNULL(iter_)
        ? OB_NOT_INIT : iter_->get_query_max_score(score);
  }

  void destroy() override
  {
    common::ObIAllocator *allocator = allocator_;
    if (OB_NOT_NULL(iter_)) {
      // The facade owns the DAS tree.  Release physical state while dynamic
      // dispatch still sees the legacy derived type, but never recurse into
      // or destruct the borrowed children.
      iter_->set_children_cnt(0);
      iter_->get_children() = nullptr;
      if (iter_->is_inited()) {
        const int ret = iter_->release();
        if (OB_SUCCESS != ret) {
        }
      }
      allocator->free(iter_);
      iter_ = nullptr;
    }
    this->~ObDASLegacyTextRetrievalEngine();
    allocator->free(this);
  }

private:
  common::ObIAllocator *allocator_;
  ObDASLegacyTRMergeIter *iter_;
  DISALLOW_COPY_AND_ASSIGN(ObDASLegacyTextRetrievalEngine);
};

int create_legacy_text_retrieval_engine(
    common::ObIAllocator &allocator,
    ObIDASTextRetrievalEngine *&engine)
{
  int ret = OB_SUCCESS;
  void *buffer = nullptr;
  engine = nullptr;
  if (OB_ISNULL(buffer = allocator.alloc(
      sizeof(ObDASLegacyTextRetrievalEngine)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    engine = new (buffer) ObDASLegacyTextRetrievalEngine(allocator);
  }
  return ret;
}

int build_legacy_text_retrieval_query(
    const ObDASIRScanCtDef *ir_ctdef,
    ObDASIRScanRtDef *ir_rtdef,
    common::ObIAllocator &allocator,
    ObArray<ObString> &query_tokens,
    ObArray<double> &boost_values,
    ObFtsEvalNode *&root_node,
    bool &has_duplicate_boolean_tokens)
{
  return ObDASLegacyTRMergeIter::build_query_tokens(
      ir_ctdef, ir_rtdef, allocator, query_tokens, boost_values, root_node,
      has_duplicate_boolean_tokens);
}

struct ObDASLegacyTextRetrievalRegistration
{
  ObDASLegacyTextRetrievalRegistration()
    : ret_(install_legacy_das_text_retrieval_engine())
  {}

  int ret_;
};

// This object is force-retained by the final composition target.  If it is
// absent, the query dispatcher fails closed with OB_NOT_INIT.
ObDASLegacyTextRetrievalRegistration g_text_retrieval_registration;

} // namespace

int install_legacy_das_text_retrieval_engine()
{
  return install_das_text_retrieval_engine_factory(
      create_legacy_text_retrieval_engine,
      build_legacy_text_retrieval_query);
}

} // namespace sql
} // namespace oceanbase
