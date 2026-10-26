// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/parser/ob_parser.h"
#include <gtest/gtest.h>
using namespace oceanbase::common;
using namespace oceanbase::sql;
TEST(FileSqlParser, ReadDiscoverDescribeCopy)
{
  for (const char *sql :
       {"SELECT * FROM read_csv('/tmp/a.csv')",
        "SELECT a.id FROM read_csv('/tmp/a.csv',header=>true,sample_size=-1) AS a",
        "SELECT * FROM read_json('/tmp/a.jsonl') j LEFT JOIN read_parquet('/tmp/a.parquet') p ON "
        "j.id=p.id",
        "SELECT * FROM read_ndjson('/tmp/a.jsonl')", "SELECT file FROM glob('/tmp/*.csv')",
        "DESCRIBE SELECT * FROM read_csv('/tmp/a.csv')",
        "COPY test_table TO '/tmp/out.csv' (FORMAT CSV, HEADER true, NULL 'nil')",
        "COPY (SELECT id FROM read_csv('/tmp/a.csv')) TO '/tmp/out.csv' (DELIMITER '|', QUOTE "
        "'\"', ESCAPE '\"')"})
  {
    SCOPED_TRACE(sql);
    ObArenaAllocator alloc;
    ObParser parser(alloc, 0);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(sql), result)) << result.error_msg_;
    ASSERT_NE(nullptr, result.result_tree_);
    ASSERT_EQ(0, result.result_tree_->children_[0]->value_);
  }
}
TEST(FileSqlParser, MalformedAndOrdinarySql)
{
  for (const char *sql : {"SELECT * FROM read_csv()", "COPY t FROM '/tmp/in.csv'",
                          "SELECT * FROM read_csv('/tmp/a',header=>)"})
  {
    ObArenaAllocator alloc;
    ObParser parser(alloc, 0);
    ParseResult result;
    EXPECT_NE(OB_SUCCESS, parser.parse(ObString(sql), result));
  }
  ObArenaAllocator alloc;
  ObParser parser(alloc, 0);
  ParseResult result;
  EXPECT_EQ(OB_SUCCESS,
            parser.parse(ObString("SELECT read_csv FROM read_json WHERE glob=1"), result));
}
int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
