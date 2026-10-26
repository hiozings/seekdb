// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "file_sql_bridge.h"
#include "sql/parser/parse_node.h"
#include "sql/session/ob_sql_session_info.h"
namespace oceanbase
{
namespace sql
{
int file_sql_options(const ParseNode *list, common::ObIAllocator &allocator,
                     common::ObString &result);
int check_file_sql_access(ObSQLSessionInfo &session, common::ObString &root);
int check_file_sql_utf8(const common::ObString &text);
int file_sql_error(const rust::String &error);
} // namespace sql
} // namespace oceanbase
