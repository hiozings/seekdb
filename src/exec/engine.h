#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sql/sql.h"

namespace mvdb {

struct ResultSet {
  bool has_rows = false;
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  int64_t affected_rows = 0;
};

namespace detail {

struct Column {
  std::string name;
  ColumnType type = ColumnType::BigInt;
  int64_t length = 0;
  int64_t dim = 0;
};

struct Row {
  std::vector<Value> values;
  bool deleted = false;
};

struct Table {
  std::string name;
  std::vector<Column> columns;
  size_t pk_index = 0;
  std::vector<Row> rows;
  std::unordered_map<int64_t, size_t> pk_index_map;
};

struct Database {
  std::map<std::string, Table> tables;  // keyed by lower-cased name
};

}  // namespace detail

// In-memory execution engine used by milestone M0 scaffolding. The durable
// page store and WAL replace the backing storage in later milestones; the
// external behaviour (statements, errors, results) stays unchanged.
class Engine {
 public:
  Engine() = default;

  void set_float_format(std::string fmt) { float_fmt_ = std::move(fmt); }

  // Executes one statement. Throws SqlError on failure; a failed statement
  // leaves no partial modification behind.
  ResultSet execute(const Statement& stmt);

 private:
  detail::Table& require_table(const std::string& name);
  size_t require_column(const detail::Table& table, const std::string& name) const;
  Value coerce(const detail::Table& table, size_t column_index, const Value& input) const;
  bool matches(const detail::Table& table, const detail::Row& row,
               const std::vector<Condition>& conds) const;

  ResultSet exec_create(const Statement& stmt);
  ResultSet exec_insert(const Statement& stmt);
  ResultSet exec_select(const Statement& stmt);
  ResultSet exec_update(const Statement& stmt);
  ResultSet exec_delete(const Statement& stmt);

  std::string format_value(const Value& value, ColumnType type) const;
  double l2_distance(const std::vector<float>& a, const std::vector<float>& b) const;

  detail::Database db_;
  bool in_txn_ = false;
  std::unique_ptr<detail::Database> txn_snapshot_;
  std::string float_fmt_ = "%.6f";
};

}  // namespace mvdb
