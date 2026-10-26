// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/file_copy_op.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
namespace oceanbase
{
namespace sql
{
using namespace common;
OB_SERIALIZE_MEMBER((FileCopySpec, ObSelectIntoSpec), column_names_);
int FileCopyOp::inner_open()
{
  int ret = OB_SUCCESS;
  ObString root;
  const auto &spec = static_cast<const FileCopySpec &>(spec_);
  if (OB_FAIL(check_file_sql_access(*ctx_.get_my_session(), root)))
  {
  }
  else
  {
    authorized_root_ = root;
    rust::Vec<rust::String> names;
    for (const auto &name : spec.column_names_)
    {
      if (OB_FAIL(check_file_sql_utf8(name)))
      {
        break;
      }
      names.push_back(rust::String(name.empty() ? "" : name.ptr(), name.length()));
    }
    ObString path = spec.outfile_name_.get_string(), options = spec.field_str_.get_string();
    if (OB_FAIL(ret))
    {
      return ret;
    }
    if (path.empty())
    {
      return OB_INVALID_ARGUMENT;
    }
    if (OB_FAIL(check_file_sql_utf8(path)) || OB_FAIL(check_file_sql_utf8(options)))
    {
      return ret;
    }
    writer_.emplace(file_sql::open_copy(
        rust::Str(root.ptr(), root.length()), rust::Str(path.ptr(), path.length()),
        rust::Str(options.ptr(), options.length()),
        rust::Slice<const rust::String>(names.data(), names.size())));
    auto error = file_sql::copy_error(**writer_);
    if (!error.empty())
    {
      ret = file_sql_error(rust::String(error.data(), error.size()));
    }
  }
  return ret;
}
int FileCopyOp::write_row(int64_t index)
{
  int ret = OB_SUCCESS;
  const auto &spec = static_cast<const FileCopySpec &>(spec_);
  ObEvalCtx::BatchInfoScopeGuard batch_guard(eval_ctx_);
  batch_guard.set_batch_idx(index);
  ObEvalCtx::TempAllocGuard temp(eval_ctx_);
  ObArenaAllocator &allocator = temp.get_allocator();
  rust::Vec<file_sql::Cell> cells;
  for (ObExpr *expr : spec.select_exprs_)
  {
    ObDatum *datum = nullptr;
    ObString text;
    if (OB_FAIL(expr->eval(eval_ctx_, datum)))
    {
      break;
    }
    if (!datum->is_null())
    {
      if (ob_is_string_type(expr->datum_meta_.type_))
      {
        ret = ObTextStringHelper::read_real_string_data(ctx_, allocator, *datum, expr->datum_meta_,
                                                        expr->obj_meta_.has_lob_header(), text);
      }
      else
      {
        ObObj source, target;
        ObCastCtx cast(&allocator, nullptr, CM_NONE, CS_TYPE_UTF8MB4_BIN);
        if (OB_FAIL(datum->to_obj(source, expr->obj_meta_)))
        {
        }
        else if (OB_FAIL(ObObjCaster::to_type(ObVarcharType, cast, source, target)))
        {
        }
        else
        {
          text = target.get_string();
        }
      }
    }
    if (OB_FAIL(ret))
    {
      break;
    }
    if (OB_FAIL(check_file_sql_utf8(text)))
    {
      break;
    }
    cells.push_back(file_sql::Cell{
        rust::String(text.ptr() == nullptr ? "" : text.ptr(), text.length()), datum->is_null()});
  }
  if (OB_SUCC(ret))
  {
    auto error = file_sql::copy_row(**writer_,
                                    rust::Slice<const file_sql::Cell>(cells.data(), cells.size()));
    if (!error.empty())
    {
      ret = file_sql_error(error);
    }
  }
  return ret;
}
int FileCopyOp::drain(bool batch)
{
  if (done_)
  {
    return OB_SUCCESS;
  }
  int ret = OB_SUCCESS;
  int64_t rows = 0;
  while (OB_SUCC(ret))
  {
    if (OB_FAIL(ctx_.check_status()))
    {
      break;
    }
    clear_evaluated_flag();
    if (batch)
    {
      const ObBatchRows *input = nullptr;
      if (OB_FAIL(child_->get_next_batch(1024, input)))
      {
        break;
      }
      ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);
      guard.set_batch_size(input->size_);
      for (int64_t i = 0; OB_SUCC(ret) && i < input->size_; ++i)
      {
        if (!input->skip_->at(i))
        {
          ret = write_row(i);
          if (OB_SUCC(ret))
          {
            ++rows;
          }
        }
      }
      if (input->end_)
      {
        break;
      }
    }
    else
    {
      ret = child_->get_next_row();
      if (ret == OB_ITER_END)
      {
        ret = OB_SUCCESS;
        break;
      }
      if (OB_SUCC(ret))
      {
        ret = write_row(0);
        if (OB_SUCC(ret))
        {
          ++rows;
        }
      }
    }
  }
  if (OB_SUCC(ret))
  {
    ObString root;
    if (OB_FAIL(check_file_sql_access(*ctx_.get_my_session(), root)))
    {
    }
    else if (root != authorized_root_)
    {
      ret = OB_ERR_NO_PRIVILEGE;
    }
    else
    {
      auto error = file_sql::copy_commit(**writer_);
      if (!error.empty())
      {
        ret = file_sql_error(error);
      }
      else
      {
        done_ = true;
        ctx_.get_physical_plan_ctx()->set_affected_rows(rows);
      }
    }
  }
  if (OB_FAIL(ret))
  {
    writer_.reset();
  }
  return ret;
}
int FileCopyOp::inner_get_next_row()
{
  int ret = drain(false);
  return ret == OB_SUCCESS ? OB_ITER_END : ret;
}
int FileCopyOp::inner_get_next_batch(int64_t)
{
  int ret = drain(true);
  brs_.size_ = 0;
  brs_.end_ = true;
  return ret;
}
int FileCopyOp::inner_close()
{
  writer_.reset();
  return OB_SUCCESS;
}
void FileCopyOp::destroy()
{
  inner_close();
  ObOperator::destroy();
}
} // namespace sql
} // namespace oceanbase
