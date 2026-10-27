// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/parser/ob_parser.h"
#include "share/schema/ai_column_schema.h"
#include <gtest/gtest.h>
using namespace oceanbase::common;
using namespace oceanbase::sql;
using namespace oceanbase::share::schema;
TEST(AiColumnParser, CreateAddModifyRetry)
{
  for (const char *sql : {
      "CREATE TABLE docs(id bigint primary key, content text, tags json AI COLUMN (AI_COMPLETE('m',concat('x', content))) WITH (sync_mode='async',max_retries=2))",
      "ALTER TABLE docs ADD COLUMN emb vector(4) AI COLUMN (AI_EMBED('m',content))",
      "ALTER TABLE docs MODIFY COLUMN tags json AI COLUMN (AI_COMPLETE('m',content))",
      "ALTER TABLE docs RETRY AI COLUMN tags WHERE status='FAILED' AND id=1",
      "ALTER TABLE docs RETRY AI COLUMN tags"}) {
    SCOPED_TRACE(sql);
    ObArenaAllocator alloc;
    ObParser parser(alloc, 0);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(sql), result)) << result.error_msg_;
    ASSERT_NE(nullptr, result.result_tree_);
  }
}
TEST(AiColumnParser, RetryCannotMixActions)
{
  for (const char *sql : {
      "ALTER TABLE docs RETRY AI COLUMN tags, emb",
      "CREATE TABLE docs(tags json AI COLUMN ())"}) {
    ObArenaAllocator alloc;
    ObParser parser(alloc, 0);
    ParseResult result;
    EXPECT_NE(OB_SUCCESS, parser.parse(ObString(sql), result));
  }
}
TEST(AiColumnSchema, DeepCopyAndSerialization)
{
  ObColumnSchemaV2 original;
  AiColumnInfo info;
  info.original_expr_ = ObString::make_string("AI_COMPLETE('m',content)");
  info.normalized_expr_ = ObString::make_string("AI_COMPLETE('m',concat(content,' Return exactly one valid JSON value.'))");
  info.max_retries_ = 2;
  info.definition_version_ = 7;
  info.state_column_id_ = 18;
  info.request_column_id_ = 19;
  info.result_spec_column_id_ = 20;
  ASSERT_EQ(OB_SUCCESS, info.dependency_column_ids_.push_back(17));
  ASSERT_EQ(OB_SUCCESS, set_ai_column_info(original, info));
  ASSERT_TRUE(original.is_ai_column());
  ObColumnSchemaV2 copy;
  ASSERT_EQ(OB_SUCCESS, copy.assign(original));
  const int64_t size = original.get_serialize_size();
  ObArenaAllocator allocator;
  char *buf = static_cast<char *>(allocator.alloc(size));
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, original.serialize(buf, size, pos));
  ASSERT_EQ(size, pos);
  ObColumnSchemaV2 restored;
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize(buf, size, pos));
  original.reset();
  for (const auto *column : {&copy, &restored}) {
    AiColumnInfo decoded;
    ASSERT_EQ(OB_SUCCESS, get_ai_column_info(*column, decoded));
    EXPECT_EQ(info.normalized_expr_, decoded.normalized_expr_);
    EXPECT_EQ(7, decoded.definition_version_);
    EXPECT_EQ(18U, decoded.state_column_id_);
    EXPECT_EQ(2, decoded.max_retries_);
    ASSERT_EQ(1, decoded.dependency_column_ids_.count());
    EXPECT_EQ(17U, decoded.dependency_column_ids_.at(0));
  }
}
TEST(AiColumnSchema, RejectCorruptMetadata)
{
  ObColumnSchemaV2 column;
  column.add_column_flag(AI_COLUMN_FLAG);
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_info_str(ObString::make_string("truncated")));
  AiColumnInfo decoded;
  EXPECT_NE(OB_SUCCESS, get_ai_column_info(column, decoded));
}
int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
