/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "share/schema/ob_column_schema.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/ob_arena_allocator.h"

namespace oceanbase { namespace share { namespace schema {
// Strings returned by get_ai_column_info borrow storage from the column schema.
// Keep the schema guard alive while using them. set_ai_column_info deep copies.
struct AiColumnInfo
{
  common::ObString original_expr_;
  common::ObString normalized_expr_;
  common::ObSEArray<uint64_t, 4> dependency_column_ids_;
  common::ObString sync_mode_;
  int64_t max_retries_ = 3;
  int64_t definition_version_ = 1;
  uint64_t state_column_id_ = 0;
  uint64_t request_column_id_ = 0;
  uint64_t result_spec_column_id_ = 0;
  AiColumnInfo() : sync_mode_(common::ObString::make_string("async")) {}
};

inline int get_ai_column_info(const ObColumnSchemaV2 &column, AiColumnInfo &info)
{
  using namespace common;
  int ret = OB_SUCCESS;
  const ObString &data = column.get_ai_column_info_str();
  const char *buf = data.ptr();
  const int64_t data_len = data.length();
  int64_t pos = 0, version = 0, count = 0;
  info.dependency_column_ids_.reset();
  if (!column.is_ai_column() || data.empty()) { ret = OB_INVALID_ARGUMENT; }
  OB_UNIS_DECODE(version);
  if (OB_SUCC(ret) && version != 1) { ret = OB_NOT_SUPPORTED; }
  LST_DO_CODE(OB_UNIS_DECODE, info.original_expr_, info.normalized_expr_, info.sync_mode_,
              info.max_retries_, info.definition_version_, info.state_column_id_,
              info.request_column_id_, info.result_spec_column_id_, count);
  if (OB_SUCC(ret) && (count < 0 || count > data_len)) { ret = OB_INVALID_ARGUMENT; }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    uint64_t id = 0;
    OB_UNIS_DECODE(id);
    if (OB_SUCC(ret)) { ret = info.dependency_column_ids_.push_back(id); }
  }
  if (OB_SUCC(ret) && pos != data_len) { ret = OB_INVALID_ARGUMENT; }
  return ret;
}

inline int set_ai_column_info(ObColumnSchemaV2 &column, const AiColumnInfo &info)
{
  using namespace common;
  int ret = OB_SUCCESS;
  int64_t version = 1, count = info.dependency_column_ids_.count(), len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN, version, info.original_expr_, info.normalized_expr_, info.sync_mode_,
              info.max_retries_, info.definition_version_, info.state_column_id_,
              info.request_column_id_, info.result_spec_column_id_, count);
  for (int64_t i = 0; i < count; ++i) { OB_UNIS_ADD_LEN(info.dependency_column_ids_.at(i)); }
  ObArenaAllocator allocator;
  const int64_t buf_len = len;
  char *buf = static_cast<char *>(allocator.alloc(len));
  int64_t pos = 0;
  if (OB_ISNULL(buf)) { ret = OB_ALLOCATE_MEMORY_FAILED; }
  LST_DO_CODE(OB_UNIS_ENCODE, version, info.original_expr_, info.normalized_expr_, info.sync_mode_,
              info.max_retries_, info.definition_version_, info.state_column_id_,
              info.request_column_id_, info.result_spec_column_id_, count);
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) { OB_UNIS_ENCODE(info.dependency_column_ids_.at(i)); }
  if (OB_SUCC(ret) && OB_SUCC(ret = column.set_ai_column_info_str(ObString(pos, buf)))) {
    column.add_column_flag(AI_COLUMN_FLAG);
  }
  return ret;
}
inline int make_ai_hidden_column(const ObColumnSchemaV2 &result, uint64_t id, int kind,
                                 ObColumnSchemaV2 &hidden)
{
  using namespace common;
  int ret = OB_SUCCESS;
  char name[96];
  snprintf(name, sizeof(name), "__ai_%lu_%s", result.get_column_id(),
           kind == 0 ? "state" : kind == 1 ? "request" : "result_spec");
  hidden.set_table_id(result.get_table_id());
  hidden.set_column_id(id);
  hidden.set_is_hidden(true);
  hidden.set_nullable(true);
  hidden.set_data_type(kind == 0 ? ObIntType : ObVarcharType);
  hidden.set_data_length(kind == 1 ? 128 : 2048);
  hidden.set_collation_type(kind == 0 ? CS_TYPE_BINARY : CS_TYPE_UTF8MB4_BIN);
  if (OB_FAIL(hidden.set_column_name(name))) {}
  else if (kind == 0) {
    ObObj pending; pending.set_int(0);
    if (OB_FAIL(hidden.set_cur_default_value(pending, false))) {}
    else { ret = hidden.set_orig_default_value(pending); }
  }
  return ret;
}
}}}
