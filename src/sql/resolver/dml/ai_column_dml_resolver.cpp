// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_del_upd_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "share/schema/ai_column_schema.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;
namespace {
int ai_uuid_expr(ObRawExprFactory &factory, ObSQLSessionInfo *session, ObRawExpr *&out)
{
  int ret = OB_SUCCESS;
  ObSysFunRawExpr *expr = nullptr;
  if (OB_FAIL(factory.create_raw_expr(T_FUN_SYS_UUID, expr))) {
  } else {
    expr->set_func_name(ObString::make_string("uuid"));
    ret = expr->formalize(session);
    out = expr;
  }
  return ret;
}
int ai_changed_expr(ObRawExprFactory &factory, ObSQLSessionInfo *session,
                    ObRawExpr *old_value, ObRawExpr *new_value, ObRawExpr *&out)
{
  int ret = OB_SUCCESS;
  // HEX makes textual comparison byte-exact even under a case-insensitive collation.
  auto exact_text = [&](ObRawExpr *&value) -> int {
    int ret = OB_SUCCESS;
    if (ob_is_string_type(value->get_data_type())) {
      ObSysFunRawExpr *hex = nullptr;
      if (OB_FAIL(factory.create_raw_expr(T_FUN_SYS_HEX, hex))) {
      } else if (OB_FAIL(hex->set_param_expr(value))) {
      } else {
        hex->set_func_name(ObString::make_string("hex"));
        ret = hex->formalize(session);
        value = hex;
      }
    }
    return ret;
  };
  ObOpRawExpr *equal = nullptr, *negate = nullptr;
  if (OB_FAIL(exact_text(old_value)) || OB_FAIL(exact_text(new_value))) {
  } else if (OB_FAIL(factory.create_raw_expr(T_OP_NSEQ, equal))) {
  } else if (OB_FAIL(equal->set_param_exprs(old_value, new_value))) {
  } else if (OB_FAIL(equal->formalize(session))) {
  } else if (OB_FAIL(factory.create_raw_expr(T_OP_NOT, negate))) {
  } else if (OB_FAIL(negate->set_param_expr(equal))) {
  } else {
    ret = negate->formalize(session);
    out = negate;
  }
  return ret;
}
}

int ObDelUpdResolver::resolve_ai_column_assignments(
    ObTableAssignment &table_assign, const TableItem &item, const ObTableSchema &schema)
{
  int ret = OB_SUCCESS;
  if (session_info_->is_ai_column_internal()) { return ret; }
  auto base_column_id = [&](const ObColumnRefRawExpr *expr) {
    const ColumnItem *item = get_stmt()->get_column_item_by_id(expr->get_table_id(), expr->get_column_id());
    return item != nullptr && item->base_cid_ != OB_INVALID_ID ? item->base_cid_ : expr->get_column_id();
  };
  const uint64_t retry_column = session_info_->get_ai_column_retry_column_id();
  auto &assignments = table_assign.assignments_;
  for (int64_t i = assignments.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
    const ObColumnSchemaV2 *column = schema.get_column_schema(base_column_id(assignments.at(i).column_expr_));
    if (column != nullptr && column->is_ai_column()) {
      if (column->get_column_id() == retry_column) {
        ret = assignments.remove(i); // Trusted RETRY placeholder; replaced below.
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "assigning an AI result column");
      }
    }
  }
  // A previously resolved hidden reference must not bypass the ordinary name lookup
  // restriction (for example through a view or an already-populated column cache).
  for (auto it = schema.column_begin(); OB_SUCC(ret) && it != schema.column_end(); ++it) {
    if (!(*it)->is_ai_column()) { continue; }
    AiColumnInfo info;
    if (OB_FAIL(get_ai_column_info(**it, info))) { break; }
    for (int64_t i = 0; OB_SUCC(ret) && i < assignments.count(); ++i) {
      const uint64_t id = base_column_id(assignments.at(i).column_expr_);
      if (!assignments.at(i).is_implicit_ && (id == info.state_column_id_
          || id == info.request_column_id_ || id == info.result_spec_column_id_)) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "assigning AI hidden columns");
      }
    }
  }
  const int64_t user_count = assignments.count();
  ObRawExpr *request = nullptr;
  for (auto it = schema.column_begin(); OB_SUCC(ret) && it != schema.column_end(); ++it) {
    const ObColumnSchemaV2 *column = *it;
    if (!column->is_ai_column() || (retry_column != 0 && retry_column != column->get_column_id())) { continue; }
    AiColumnInfo info;
    ObSEArray<ObRawExpr *, 8> changed;
    ObRawExpr *condition = nullptr;
    if (OB_FAIL(get_ai_column_info(*column, info))) { break; }
    if (retry_column != 0) {
      ObConstRawExpr *yes = nullptr;
      ret = ObRawExprUtils::build_const_int_expr(*params_.expr_factory_, ObIntType, 1, yes);
      condition = yes;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < user_count; ++i) {
        const auto &assign = assignments.at(i);
        const uint64_t id = base_column_id(assign.column_expr_);
        const auto *dependency = schema.get_column_schema(id);
        bool depends = dependency != nullptr && dependency->is_rowkey_column();
        for (int64_t j = 0; j < info.dependency_column_ids_.count(); ++j) {
          depends |= id == info.dependency_column_ids_.at(j);
        }
        if (depends) {
          ObRawExpr *different = nullptr;
          if (OB_FAIL(ai_changed_expr(*params_.expr_factory_, session_info_, assign.column_expr_, assign.expr_, different))) {
          } else { ret = changed.push_back(different); }
        }
      }
      if (OB_SUCC(ret) && changed.empty()) { continue; }
      if (OB_SUCC(ret)) { ret = ObRawExprUtils::build_or_exprs(*params_.expr_factory_, changed, condition); }
    }
    if (OB_SUCC(ret) && request == nullptr) { ret = ai_uuid_expr(*params_.expr_factory_, session_info_, request); }
    const uint64_t ids[] = {column->get_column_id(), info.state_column_id_, info.request_column_id_, info.result_spec_column_id_};
    for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
      const auto *target = schema.get_column_schema(ids[i]);
      ObSEArray<ObColumnRefRawExpr *, 1> refs;
      ObRawExpr *new_value = nullptr;
      if (target == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(add_column_to_stmt(item, *target, refs))) { }
      else if (refs.count() != 1) { ret = OB_ERR_UNEXPECTED; }
      else if (i == 0) { ret = ObRawExprUtils::build_null_expr(*params_.expr_factory_, new_value); }
      else if (i == 1) {
        ObConstRawExpr *zero = nullptr;
        ret = ObRawExprUtils::build_const_int_expr(*params_.expr_factory_, ObIntType, 0, zero);
        new_value = zero;
      } else if (i == 2) { new_value = request; }
      else {
        ObSqlString version;
        ObString saved;
        ObConstRawExpr *constant = nullptr;
        if (OB_FAIL(version.append_fmt("%ld", info.definition_version_))) { }
        else if (OB_FAIL(ob_write_string(*allocator_, version.string(), saved))) { }
        else { ret = ObRawExprUtils::build_const_string_expr(*params_.expr_factory_, ObVarcharType, saved, CS_TYPE_UTF8MB4_BIN, constant); }
        new_value = constant;
      }
      ObAssignment assignment;
      if (OB_SUCC(ret)) {
        assignment.column_expr_ = refs.at(0);
        assignment.is_implicit_ = true;
        if (OB_FAIL(ObRawExprUtils::build_case_when_expr(*params_.expr_factory_, condition, new_value, refs.at(0), assignment.expr_))) { }
        else if (OB_FAIL(assignment.expr_->formalize(session_info_))) { }
        else if (OB_FAIL(ObRawExprUtils::build_column_conv_expr(*params_.expr_factory_, *allocator_, *refs.at(0), assignment.expr_, session_info_))) { }
        else { ret = assignments.push_back(assignment); }
      }
    }
    if (OB_SUCC(ret)) { get_stmt()->get_query_ctx()->disable_udf_parallel_ = true; }
  }
  return ret;
}

int ObDelUpdResolver::resolve_ai_column_insert(ObInsertTableInfo &table_info)
{
  int ret = OB_SUCCESS;
  if (session_info_->is_ai_column_internal()) { return ret; }
  auto base_column_id = [&](const ObColumnRefRawExpr *expr) {
    const ColumnItem *item = get_stmt()->get_column_item_by_id(expr->get_table_id(), expr->get_column_id());
    return item != nullptr && item->base_cid_ != OB_INVALID_ID ? item->base_cid_ : expr->get_column_id();
  };
  const ObTableSchema *schema = nullptr;
  if (OB_FAIL(schema_checker_->get_table_schema(table_info.ref_table_id_, schema))) { return ret; }
  if (schema == nullptr) { return OB_ERR_UNEXPECTED; }
  ObRawExpr *request = nullptr;
  for (auto it = schema->column_begin(); OB_SUCC(ret) && it != schema->column_end(); ++it) {
    const auto *column = *it;
    if (!column->is_ai_column()) { continue; }
    for (int64_t i = 0; i < table_info.values_desc_.count(); ++i) {
      if (base_column_id(table_info.values_desc_.at(i)) == column->get_column_id()) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "assigning an AI result column");
      }
    }
    AiColumnInfo info;
    if (OB_SUCC(ret)) { ret = get_ai_column_info(*column, info); }
    for (int64_t i = 0; OB_SUCC(ret) && i < table_info.values_desc_.count(); ++i) {
      const uint64_t id = base_column_id(table_info.values_desc_.at(i));
      if (id == info.state_column_id_ || id == info.request_column_id_ || id == info.result_spec_column_id_) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "assigning AI hidden columns");
      }
    }
    if (OB_SUCC(ret) && request == nullptr) { ret = ai_uuid_expr(*params_.expr_factory_, session_info_, request); }
    for (int64_t i = 0; OB_SUCC(ret) && i < table_info.column_exprs_.count(); ++i) {
      ObColumnRefRawExpr *target = table_info.column_exprs_.at(i);
      const uint64_t id = base_column_id(target);
      ObRawExpr *value = nullptr;
      if (id == info.request_column_id_) { value = request; }
      else if (id == column->get_column_id()) { ret = ObRawExprUtils::build_null_expr(*params_.expr_factory_, value); }
      else if (id == info.state_column_id_) {
        ObConstRawExpr *zero = nullptr;
        ret = ObRawExprUtils::build_const_int_expr(*params_.expr_factory_, ObIntType, 0, zero);
        value = zero;
      } else if (id == info.result_spec_column_id_) {
        ObSqlString version;
        ObString saved;
        ObConstRawExpr *constant = nullptr;
        if (OB_FAIL(version.append_fmt("%ld", info.definition_version_))) { }
        else if (OB_FAIL(ob_write_string(*allocator_, version.string(), saved))) { }
        else { ret = ObRawExprUtils::build_const_string_expr(*params_.expr_factory_, ObVarcharType, saved, CS_TYPE_UTF8MB4_BIN, constant); }
        value = constant;
      }
      if (OB_SUCC(ret) && value != nullptr) {
        if (OB_FAIL(ObRawExprUtils::build_column_conv_expr(*params_.expr_factory_, *allocator_, *target, value, session_info_))) { }
        else { table_info.column_conv_exprs_.at(i) = value; }
      }
    }
    if (OB_SUCC(ret)) { get_stmt()->get_query_ctx()->disable_udf_parallel_ = true; }
  }
  return ret;
}
}} // oceanbase::sql
