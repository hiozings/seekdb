// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/file_scan/file_scan_utils.h"
#include "common/json_type/ob_json_tree.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_schema_utils.h"
namespace oceanbase
{
namespace sql
{
int file_sql_options(const ParseNode *list, common::ObIAllocator &allocator,
                     common::ObString &result)
{
  using namespace common;
  int ret = OB_SUCCESS;
  ObJsonObject options(&allocator);
  for (int i = 0; OB_SUCC(ret) && list != nullptr && i < list->num_child_; ++i)
  {
    const ParseNode *option = list->children_[i];
    ObString key(option->children_[0]->str_len_, option->children_[0]->str_value_);
    const ParseNode *value = option->children_[1];
    ObJsonNode *json = nullptr;
    if (options.get_value(key) != nullptr)
    {
      ret = OB_INVALID_ARGUMENT;
    }
    else if (value->type_ == T_VARCHAR)
    {
      json = OB_NEWx(ObJsonString, &allocator, ObString(value->str_len_, value->str_value_));
    }
    else if (value->type_ == T_BOOL)
    {
      json = OB_NEWx(ObJsonBoolean, &allocator, value->value_ != 0);
    }
    else if (value->type_ == T_INT)
    {
      json = OB_NEWx(ObJsonInt, &allocator, value->value_);
    }
    else
    {
      ret = OB_INVALID_ARGUMENT;
    }
    if (OB_SUCC(ret))
    {
      ret = json == nullptr ? OB_ALLOCATE_MEMORY_FAILED : options.add(key, json);
    }
  }
  ObJsonBuffer buffer(&allocator);
  if (OB_SUCC(ret) && OB_SUCC(ret = options.print(buffer, 0)))
  {
    result = buffer.string();
    ret = check_file_sql_utf8(result);
  }
  return ret;
}
int check_file_sql_utf8(const common::ObString &text)
{
  using namespace common;
  if (text.empty())
  {
    return OB_SUCCESS;
  }
  int64_t length = 0;
  int32_t error = 0;
  int ret =
      ObCharset::well_formed_len(CS_TYPE_UTF8MB4_BIN, text.ptr(), text.length(), length, error);
  if (ret != OB_SUCCESS || error != 0 || length != text.length())
  {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "File SQL requires valid UTF-8 text");
  }
  return ret;
}
int file_sql_error(const rust::String &error)
{
  const int ret = common::OB_INVALID_ARGUMENT;
  rust::String message(error);
  LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.c_str());
  return ret;
}
int check_file_sql_access(ObSQLSessionInfo &session, common::ObString &root)
{
  int ret = common::OB_SUCCESS;
  int64_t enabled = 0;
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled)))
  {
  }
  else if (!enabled)
  {
    ret = common::OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL is disabled; enable_file_sql is required");
  }
  else if ((session.get_user_priv_set() & OB_PRIV_FILE) == 0)
  {
    ret = common::OB_ERR_NO_PRIVILEGE;
  }
  else
  {
    if (GCTX.schema_service_ == nullptr || session.get_cur_exec_ctx() == nullptr)
    {
      ret = common::OB_ERR_UNEXPECTED;
    }
    else
    {
      ret = share::schema::ObSchemaUtils::get_runtime_varchar_variable(
          *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV,
          session.get_cur_exec_ctx()->get_allocator(), root);
    }
    if (OB_SUCC(ret))
    {
      ret = check_file_sql_utf8(root);
    }
    if (OB_SUCC(ret) && (root.empty() || root.case_compare("NULL") == 0))
    {
      ret = common::OB_ERR_NO_PRIVILEGE;
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
