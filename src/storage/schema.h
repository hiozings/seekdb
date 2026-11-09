#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sql/sql.h"

namespace mvdb {

struct Column {
  std::string name;
  ColumnType type = ColumnType::BigInt;
  int64_t length = 0;  // VARCHAR(n) character count
  int64_t dim = 0;     // VECTOR(n) dimension
};

struct TableMeta {
  uint32_t id = 0;
  std::string name;
  std::vector<Column> columns;
  size_t pk_index = 0;
};

}  // namespace mvdb
