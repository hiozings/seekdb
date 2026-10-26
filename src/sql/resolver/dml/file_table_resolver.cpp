// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#define USING_LOG_PREFIX SQL_RESV
#include "common/json_type/ob_json_tree.h"
#include "share/json/ob_json_helper.h"
#include "sql/engine/basic/file_scan/file_scan_utils.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
namespace oceanbase
{
namespace sql
{
using namespace common;
int ObDMLResolver::resolve_file_table_item(const ParseNode &node, TableItem *&item)
{
  int ret = OB_SUCCESS;
  ObString root;
  if (node.num_child_ != 4 || node.children_[1] == nullptr || node.children_[1]->type_ != T_VARCHAR)
  {
    ret = OB_INVALID_ARGUMENT;
  }
  else if (OB_FAIL(check_file_sql_access(*session_info_, root)))
  {
  }
  else
  {
    ObString format(node.children_[0]->str_len_, node.children_[0]->str_value_);
    ObString path(node.children_[1]->str_len_, node.children_[1]->str_value_);
    ObJsonObject options(allocator_);
    ParseNode *list = node.children_[2];
    for (int i = 0; OB_SUCC(ret) && list != nullptr && i < list->num_child_; ++i)
    {
      ParseNode *option = list->children_[i];
      ObString key(option->children_[0]->str_len_, option->children_[0]->str_value_);
      ParseNode *value = option->children_[1];
      ObJsonNode *json = nullptr;
      if (options.get_value(key) != nullptr)
      {
        ret = OB_INVALID_ARGUMENT;
      }
      else if (value->type_ == T_VARCHAR)
      {
        json = OB_NEWx(ObJsonString, allocator_, ObString(value->str_len_, value->str_value_));
      }
      else if (value->type_ == T_BOOL)
      {
        json = OB_NEWx(ObJsonBoolean, allocator_, value->value_ != 0);
      }
      else if (value->type_ == T_INT)
      {
        json = OB_NEWx(ObJsonInt, allocator_, value->value_);
      }
      else
      {
        ret = OB_INVALID_ARGUMENT;
      }
      if (OB_SUCC(ret))
      {
        if (json == nullptr)
        {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        }
        else
        {
          ret = options.add(key, json);
        }
      }
    }
    ObJsonBuffer buffer(allocator_);
    if (OB_FAIL(ret))
    {
    }
    else if (OB_FAIL(options.print(buffer, 0)))
    {
    }
    else if (path.empty())
    {
      ret = OB_INVALID_ARGUMENT;
    }
    else if (OB_FAIL(check_file_sql_utf8(path)))
    {
    }
    else if (OB_FAIL(check_file_sql_utf8(buffer.string())))
    {
    }
    else
    {
      const int64_t budget =
          THIS_WORKER.is_timeout_ts_valid() ? THIS_WORKER.get_timeout_remain() : 60000000;
      auto binding = file_sql::file_bind_deadline(
          rust::Str(root.ptr(), root.length()), rust::Str(path.ptr(), path.length()),
          rust::Str(format.ptr(), format.length()), rust::Str(buffer.ptr(), buffer.length()),
          static_cast<uint64_t>(std::max<int64_t>(0, budget)));
      if (!binding.error.empty())
      {
        ret = file_sql_error(binding.error);
      }
      else
      {
        ObString metadata, alias;
        ObConstRawExpr *value = nullptr;
        if (OB_FAIL(ob_write_string(*allocator_, ObString(binding.spec.size(), binding.spec.data()),
                                    metadata)))
        {
        }
        else if (OB_FAIL(ObRawExprUtils::build_const_string_expr(
                     *params_.expr_factory_, ObVarcharType, metadata, CS_TYPE_UTF8MB4_BIN, value)))
        {
        }
        else if (OB_ISNULL(item = get_stmt()->create_table_item(*allocator_)))
        {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        }
        else
        {
          if (node.children_[3] != nullptr)
          {
            alias.assign_ptr(node.children_[3]->str_value_, node.children_[3]->str_len_);
          }
          else
          {
            alias = format;
          }
          item->type_ = TableItem::FILE_TABLE;
          item->table_id_ = generate_table_id();
          item->table_name_ = alias;
          item->alias_name_ = alias;
          item->function_table_expr_ = value;
          ret = get_stmt()->add_table_item(session_info_, item);
          const ObObjType types[] = {ObNullType, ObTinyIntType,  ObIntType,    ObDoubleType,
                                     ObDateType, ObDateTimeType, ObVarcharType};
          for (size_t i = 0; OB_SUCC(ret) && i < binding.names.size(); ++i)
          {
            ObObjMeta meta;
            meta.set_type(types[binding.kinds[i]]);
            meta.set_collation_type(meta.get_type() == ObVarcharType ? CS_TYPE_UTF8MB4_BIN
                                                                     : CS_TYPE_BINARY);
            ObAccuracy accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[meta.get_type()];
            if (meta.get_type() == ObDateTimeType)
            {
              accuracy.set_scale(6);
            }
            if (meta.get_type() == ObVarcharType)
            {
              accuracy.set_length(16 * 1024 * 1024);
            }
            ObString name;
            ColumnItem *column = nullptr;
            if (OB_FAIL(ob_write_string(
                    *allocator_, ObString(binding.names[i].size(), binding.names[i].data()), name)))
            {
            }
            else
            {
              ret = resolve_function_table_column_item(*item, meta, accuracy, name,
                                                       OB_APP_MIN_COLUMN_ID + i, column);
            }
            if (OB_SUCC(ret))
            {
              column->expr_->set_collation_level(CS_LEVEL_IMPLICIT);
              column->expr_->clear_explicited_referece();
            }
          }
          get_stmt()->get_query_ctx()->disable_udf_parallel_ = true;
          get_stmt()->get_query_ctx()->has_file_sql_ = true;
        }
      }
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
