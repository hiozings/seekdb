// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/file_scan_op.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
namespace oceanbase
{
namespace sql
{
using namespace common;
OB_SERIALIZE_MEMBER((FileScanSpec, ObFunctionTableSpec), column_indexes_);
int FileScanOp::inner_open()
{
  int ret = OB_SUCCESS;
  const auto &spec = static_cast<const FileScanSpec &>(spec_);
  ObString root;
  ObDatum *metadata = nullptr;
  if (OB_FAIL(check_file_sql_access(*ctx_.get_my_session(), root)))
  {
  }
  else if (OB_FAIL(spec.value_expr_->eval(eval_ctx_, metadata)))
  {
  }
  else
  {
    rust::Vec<size_t> columns;
    for (int64_t index : spec.column_indexes_)
    {
      columns.push_back(index);
    }
    ObString value = metadata->get_string();
    reader_.emplace(file_sql::open_file_scan(
        rust::Str(root.ptr(), root.length()), rust::Str(value.ptr(), value.length()),
        rust::Slice<const size_t>(columns.data(), columns.size())));
    auto error = file_sql::scan_error(**reader_);
    if (!error.empty())
    {
      ret = file_sql_error(rust::String(error.data(), error.size()));
    }
  }
  position_ = 0;
  return ret;
}
int FileScanOp::fill_row(size_t row, int64_t output_index)
{
  int ret = OB_SUCCESS;
  const auto &spec = static_cast<const FileScanSpec &>(spec_);
  ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);
  guard.set_batch_idx(output_index);
  for (int64_t i = 0; OB_SUCC(ret) && i < spec.column_exprs_.count(); ++i)
  {
    auto &cell = batch_.cells[row * spec.column_exprs_.count() + i];
    ObExpr *expr = spec.column_exprs_.at(i);
    ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    if (cell.is_null)
    {
      datum.set_null();
    }
    else
    {
      ObObj source, target;
      source.set_varchar(ObString(cell.text.size(), cell.text.data()));
      source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      ObCastCtx cast(&ctx_.get_allocator(), nullptr, CM_NONE, CS_TYPE_UTF8MB4_BIN);
      if (OB_FAIL(ObObjCaster::to_type(expr->datum_meta_.type_, cast, source, target)))
      {
      }
      else
      {
        ret = datum.from_obj(target);
      }
    }
    if (OB_SUCC(ret))
    {
      expr->set_evaluated_flag(eval_ctx_);
      expr->set_evaluated_projected(eval_ctx_);
    }
  }
  return ret;
}
int FileScanOp::inner_get_next_row()
{
  int ret = ctx_.check_status();
  clear_evaluated_flag();
  if (OB_SUCC(ret) && position_ >= batch_.rows)
  {
    batch_ = file_sql::Batch();
    batch_ = file_sql::scan_next(**reader_, 1);
    position_ = 0;
    if (!batch_.error.empty())
    {
      ret = file_sql_error(batch_.error);
    }
  }
  if (OB_SUCC(ret))
  {
    ret = batch_.rows == 0 ? OB_ITER_END : fill_row(position_++, 0);
  }
  return ret;
}
int FileScanOp::inner_get_next_batch(int64_t max_rows)
{
  int ret = ctx_.check_status();
  clear_evaluated_flag();
  if (OB_SUCC(ret))
  {
    batch_ = file_sql::Batch();
    batch_ = file_sql::scan_next(**reader_, std::min<int64_t>(max_rows, 1024));
    if (!batch_.error.empty())
    {
      ret = file_sql_error(batch_.error);
    }
    else
    {
      brs_.size_ = batch_.rows;
      brs_.end_ = batch_.eof;
      brs_.skip_->reset(brs_.size_);
      ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);
      guard.set_batch_size(brs_.size_);
      for (int64_t i = 0; OB_SUCC(ret) && i < brs_.size_; ++i)
      {
        ret = fill_row(i, i);
      }
    }
  }
  return ret;
}
int FileScanOp::inner_rescan()
{
  int ret = ObOperator::inner_rescan();
  reader_.reset();
  batch_ = file_sql::Batch();
  position_ = 0;
  if (OB_SUCC(ret))
  {
    ret = inner_open();
  }
  return ret;
}

int FileScanOp::inner_close()
{
  reader_.reset();
  batch_ = file_sql::Batch();
  position_ = 0;
  return OB_SUCCESS;
}
void FileScanOp::destroy()
{
  inner_close();
  ObOperator::destroy();
}
} // namespace sql
} // namespace oceanbase
