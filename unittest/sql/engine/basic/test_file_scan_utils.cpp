// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "file_sql_bridge.h"
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>
using namespace oceanbase;
TEST(FileScanUtils, CsvProjectionRescanAndChange)
{
  char path[] = "/tmp/seekdb-file-utils-XXXXXX";
  ASSERT_NE(nullptr, mkdtemp(path));
  std::string file = std::string(path) + "/a.csv";
  {
    std::ofstream out(file);
    out << "id,text\r\n1,\"hello\nworld\"\r\n2,\n";
  }
  auto binding = file_sql::file_bind(path, "a.csv", "csv", "{}");
  ASSERT_TRUE(binding.error.empty());
  ASSERT_EQ(2, binding.names.size());
  size_t column = 1;
  auto scan = file_sql::open_file_scan(path, rust::Str(binding.spec.data(), binding.spec.size()),
                                       rust::Slice<const size_t>(&column, 1));
  ASSERT_TRUE(file_sql::scan_error(*scan).empty());
  auto batch = file_sql::scan_next(*scan, 10);
  ASSERT_TRUE(batch.error.empty());
  ASSERT_EQ(2, batch.rows);
  EXPECT_EQ("hello\nworld", std::string(batch.cells[0].text));
  EXPECT_TRUE(batch.cells[1].is_null);
  EXPECT_TRUE(file_sql::scan_rescan(*scan).empty());
  {
    std::ofstream out(file);
    out << "changed";
  }
  EXPECT_FALSE(file_sql::scan_rescan(*scan).empty());
  unlink(file.c_str());
  rmdir(path);
}
TEST(FileScanUtils, DenyTraversalAndLinks)
{
  char path[] = "/tmp/seekdb-file-safe-XXXXXX";
  ASSERT_NE(nullptr, mkdtemp(path));
  EXPECT_FALSE(file_sql::file_bind(path, "../etc/passwd", "csv", "{}").error.empty());
  EXPECT_FALSE(file_sql::file_bind("", "a.csv", "csv", "{}").error.empty());
  std::string link = std::string(path) + "/link.csv";
  ASSERT_EQ(0, symlink("/etc/passwd", link.c_str()));
  EXPECT_FALSE(file_sql::file_bind(path, "link.csv", "csv", "{}").error.empty());
  unlink(link.c_str());
  rmdir(path);
}
int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
