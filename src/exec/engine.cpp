#include "exec/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

#include "util/util.h"

namespace mvdb {

namespace {

std::string lower(std::string_view s) { return to_lower(s); }

bool value_is_numeric(const Value& v) {
  return v.kind == Value::Kind::Int || v.kind == Value::Kind::Float;
}

double value_as_double(const Value& v) {
  return v.kind == Value::Kind::Int ? static_cast<double>(v.i) : v.d;
}

struct SortKeySpec {
  int out_index = -1;    // index into the projected output row
  int table_index = -1;  // index into the base table row
  bool asc = true;
};

int compare_values(const Value& a, const Value& b) {
  if (value_is_numeric(a) && value_is_numeric(b)) {
    double x = value_as_double(a);
    double y = value_as_double(b);
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
  }
  if (a.kind == Value::Kind::Str && b.kind == Value::Kind::Str) {
    int cmp = a.s.compare(b.s);
    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
    return 0;
  }
  throw_sql_error(kErrNotSupported, "ORDER BY on this value type is not supported");
}

}  // namespace

detail::Table& Engine::require_table(const std::string& name) {
  auto it = db_.tables.find(lower(name));
  if (it == db_.tables.end()) {
    throw_sql_error(kErrTableNotFound, "Table '" + name + "' doesn't exist");
  }
  return it->second;
}

size_t Engine::require_column(const detail::Table& table, const std::string& name) const {
  for (size_t i = 0; i < table.columns.size(); ++i) {
    if (table.columns[i].name == name) return i;
  }
  for (size_t i = 0; i < table.columns.size(); ++i) {
    if (iequals(table.columns[i].name, name)) return i;
  }
  throw_sql_error(kErrUnknownColumn, "Unknown column '" + name + "'");
}

Value Engine::coerce(const detail::Table& table, size_t column_index, const Value& input) const {
  const detail::Column& col = table.columns[column_index];
  switch (col.type) {
    case ColumnType::BigInt: {
      if (input.kind == Value::Kind::Int) return input;
      if (input.kind == Value::Kind::Str) {
        int64_t v = 0;
        if (!parse_int64(input.s, v)) {
          throw_sql_error(kErrIncorrectValue, "Incorrect integer value for column '" + col.name + "'");
        }
        return Value::make_int(v);
      }
      throw_sql_error(kErrIncorrectValue, "Incorrect value for BIGINT column '" + col.name + "'");
    }
    case ColumnType::Varchar: {
      std::string text;
      if (input.kind == Value::Kind::Str) {
        text = input.s;
      } else if (input.kind == Value::Kind::Int) {
        text = std::to_string(input.i);
      } else {
        throw_sql_error(kErrIncorrectValue, "Incorrect value for VARCHAR column '" + col.name + "'");
      }
      if (!is_valid_utf8(text)) {
        throw_sql_error(kErrIncorrectValue, "Invalid UTF-8 for column '" + col.name + "'");
      }
      size_t chars = utf8_codepoint_count(text);
      if (static_cast<int64_t>(chars) > col.length) {
        throw_sql_error(kErrOutOfRange,
                        "Data too long for column '" + col.name + "' (" + std::to_string(chars) +
                            " > " + std::to_string(col.length) + " characters)");
      }
      return Value::make_str(std::move(text));
    }
    case ColumnType::Vector: {
      std::vector<float> vec;
      if (input.kind == Value::Kind::Vec) {
        vec = input.v;
      } else if (input.kind == Value::Kind::Str) {
        vec = parse_vector_literal(input.s);
      } else {
        throw_sql_error(kErrIncorrectValue, "Incorrect value for VECTOR column '" + col.name + "'");
      }
      if (static_cast<int64_t>(vec.size()) != col.dim) {
        throw_sql_error(kErrIncorrectValue,
                        "Invalid vector dimension for column '" + col.name + "': expected " +
                            std::to_string(col.dim) + ", got " + std::to_string(vec.size()));
      }
      return Value::make_vec(std::move(vec));
    }
  }
  throw_sql_error(kErrInternal, "unreachable coercion path");
}

bool Engine::matches(const detail::Table& table, const detail::Row& row,
                     const std::vector<Condition>& conds) const {
  for (const Condition& cond : conds) {
    size_t idx = require_column(table, cond.column);
    const Value& actual = row.values[idx];
    Value expected = coerce(table, idx, cond.literal);
    if (actual.kind == Value::Kind::Vec || expected.kind == Value::Kind::Vec) {
      throw_sql_error(kErrNotSupported, "comparison on VECTOR columns is not supported");
    }
    int cmp = compare_values(actual, expected);
    bool ok = false;
    switch (cond.op) {
      case Condition::Op::Eq: ok = cmp == 0; break;
      case Condition::Op::Lt: ok = cmp < 0; break;
      case Condition::Op::Le: ok = cmp <= 0; break;
      case Condition::Op::Gt: ok = cmp > 0; break;
      case Condition::Op::Ge: ok = cmp >= 0; break;
    }
    if (!ok) return false;
  }
  return true;
}

ResultSet Engine::exec_create(const Statement& stmt) {
  const std::string key = lower(stmt.table);
  if (db_.tables.count(key) != 0) {
    throw_sql_error(kErrTableExists, "Table '" + stmt.table + "' already exists");
  }
  detail::Table table;
  table.name = stmt.table;
  std::set<std::string> seen;
  for (const ColumnDef& def : stmt.columns) {
    if (!seen.insert(lower(def.name)).second) {
      throw_sql_error(kErrDuplicateColumn, "Duplicate column name '" + def.name + "'");
    }
    detail::Column col;
    col.name = def.name;
    col.type = def.type;
    col.length = def.length;
    col.dim = def.dim;
    if (def.primary_key) table.pk_index = table.columns.size();
    table.columns.push_back(std::move(col));
  }
  db_.tables.emplace(key, std::move(table));
  return ResultSet{};
}

ResultSet Engine::exec_insert(const Statement& stmt) {
  detail::Table& table = require_table(stmt.table);
  std::vector<size_t> targets;
  if (stmt.insert_columns.empty()) {
    for (size_t i = 0; i < table.columns.size(); ++i) targets.push_back(i);
  } else {
    std::set<size_t> seen;
    for (const std::string& name : stmt.insert_columns) {
      size_t idx = require_column(table, name);
      if (!seen.insert(idx).second) {
        throw_sql_error(kErrIncorrectValue, "Column '" + name + "' specified twice");
      }
      targets.push_back(idx);
    }
    if (targets.size() != table.columns.size()) {
      throw_sql_error(kErrIncorrectValue,
                      "Column count doesn't match value count: every column must be provided");
    }
  }

  for (const std::vector<Value>& input_row : stmt.rows) {
    if (input_row.size() != targets.size()) {
      throw_sql_error(kErrIncorrectValue, "Column count doesn't match value count");
    }
    detail::Row row;
    row.values.resize(table.columns.size());
    for (size_t i = 0; i < targets.size(); ++i) {
      row.values[targets[i]] = coerce(table, targets[i], input_row[i]);
    }
    int64_t pk = row.values[table.pk_index].i;
    if (table.pk_index_map.count(pk) != 0) {
      throw_sql_error(kErrDuplicateKey, "Duplicate entry '" + std::to_string(pk) + "' for key 'PRIMARY'");
    }
    table.pk_index_map.emplace(pk, table.rows.size());
    table.rows.push_back(std::move(row));
  }
  ResultSet rs;
  rs.affected_rows = static_cast<int64_t>(stmt.rows.size());
  return rs;
}

ResultSet Engine::exec_select(const Statement& stmt) {
  detail::Table& table = require_table(stmt.table);

  struct OutputSpec {
    int table_index = -1;
    bool is_distance = false;
    std::vector<float> query;
    std::string name;
  };
  std::vector<OutputSpec> specs;
  if (stmt.select_star) {
    for (size_t i = 0; i < table.columns.size(); ++i) {
      OutputSpec spec;
      spec.table_index = static_cast<int>(i);
      spec.name = table.columns[i].name;
      specs.push_back(std::move(spec));
    }
  } else {
    for (const SelectItem& item : stmt.select_items) {
      OutputSpec spec;
      spec.table_index = static_cast<int>(require_column(table, item.column));
      spec.is_distance = item.is_distance;
      spec.query = item.query;
      spec.name = item.alias;
      if (spec.name.empty()) {
        spec.name = item.is_distance ? "distance" : table.columns[spec.table_index].name;
      }
      if (item.is_distance) {
        if (table.columns[spec.table_index].type != ColumnType::Vector) {
          throw_sql_error(kErrIncorrectValue, "L2_DISTANCE requires a VECTOR column");
        }
        if (static_cast<int64_t>(item.query.size()) != table.columns[spec.table_index].dim) {
          throw_sql_error(kErrIncorrectValue, "L2_DISTANCE query vector dimension mismatch");
        }
      }
      specs.push_back(std::move(spec));
    }
  }

  std::vector<SortKeySpec> sort_keys;
  for (const OrderKey& key : stmt.order_by) {
    SortKeySpec spec;
    spec.asc = key.asc;
    for (size_t i = 0; i < specs.size(); ++i) {
      if (iequals(specs[i].name, key.name)) {
        spec.out_index = static_cast<int>(i);
        break;
      }
    }
    if (spec.out_index < 0) {
      spec.table_index = static_cast<int>(require_column(table, key.name));
    }
    sort_keys.push_back(spec);
  }

  struct OutRow {
    std::vector<Value> raw;
    int64_t pk = 0;
    size_t base_row = 0;
  };
  std::vector<OutRow> out_rows;
  for (size_t row_index = 0; row_index < table.rows.size(); ++row_index) {
    const detail::Row& row = table.rows[row_index];
    if (row.deleted) continue;
    if (stmt.has_where && !matches(table, row, stmt.where)) continue;
    OutRow out;
    out.base_row = row_index;
    out.pk = row.values[table.pk_index].i;
    out.raw.reserve(specs.size());
    for (const OutputSpec& spec : specs) {
      if (spec.is_distance) {
        const std::vector<float>& stored = row.values[spec.table_index].v;
        Value v;
        v.kind = Value::Kind::Float;
        v.d = l2_distance(stored, spec.query);
        out.raw.push_back(std::move(v));
      } else {
        out.raw.push_back(row.values[spec.table_index]);
      }
    }
    out_rows.push_back(std::move(out));
  }

  if (!sort_keys.empty()) {
    std::stable_sort(out_rows.begin(), out_rows.end(), [&](const OutRow& a, const OutRow& b) {
      for (const SortKeySpec& key : sort_keys) {
        const Value& va = key.out_index >= 0
                              ? a.raw[static_cast<size_t>(key.out_index)]
                              : table.rows[a.base_row].values[static_cast<size_t>(key.table_index)];
        const Value& vb = key.out_index >= 0
                              ? b.raw[static_cast<size_t>(key.out_index)]
                              : table.rows[b.base_row].values[static_cast<size_t>(key.table_index)];
        int cmp = compare_values(va, vb);
        if (!key.asc) cmp = -cmp;
        if (cmp != 0) return cmp < 0;
      }
      return a.pk < b.pk;
    });
  }

  if (stmt.has_limit) {
    if (stmt.limit < 0) throw_sql_error(kErrOutOfRange, "LIMIT must be non-negative");
    if (static_cast<int64_t>(out_rows.size()) > stmt.limit) {
      out_rows.resize(static_cast<size_t>(stmt.limit));
    }
  }

  ResultSet rs;
  rs.has_rows = true;
  for (const OutputSpec& spec : specs) rs.columns.push_back(spec.name);
  rs.rows.reserve(out_rows.size());
  for (const OutRow& out : out_rows) {
    std::vector<std::string> line;
    line.reserve(specs.size());
    for (size_t i = 0; i < specs.size(); ++i) {
      line.push_back(format_value(out.raw[i], table.columns[specs[i].table_index].type));
    }
    rs.rows.push_back(std::move(line));
  }
  return rs;
}

ResultSet Engine::exec_update(const Statement& stmt) {
  detail::Table& table = require_table(stmt.table);
  std::vector<std::pair<size_t, Value>> assignments;
  for (const auto& item : stmt.set_items) {
    size_t idx = require_column(table, item.first);
    if (idx == table.pk_index) {
      throw_sql_error(kErrNotSupported, "updating the primary key is not supported");
    }
    assignments.emplace_back(idx, coerce(table, idx, item.second));
  }

  int64_t affected = 0;
  for (detail::Row& row : table.rows) {
    if (row.deleted) continue;
    if (stmt.has_where && !matches(table, row, stmt.where)) continue;
    for (const auto& assignment : assignments) {
      row.values[assignment.first] = assignment.second;
    }
    ++affected;
  }
  ResultSet rs;
  rs.affected_rows = affected;
  return rs;
}

ResultSet Engine::exec_delete(const Statement& stmt) {
  detail::Table& table = require_table(stmt.table);
  int64_t affected = 0;
  for (detail::Row& row : table.rows) {
    if (row.deleted) continue;
    if (stmt.has_where && !matches(table, row, stmt.where)) continue;
    row.deleted = true;
    table.pk_index_map.erase(row.values[table.pk_index].i);
    ++affected;
  }
  ResultSet rs;
  rs.affected_rows = affected;
  return rs;
}

double Engine::l2_distance(const std::vector<float>& a, const std::vector<float>& b) const {
  double sum = 0.0;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += diff * diff;
  }
  return std::sqrt(sum);
}

std::string Engine::format_value(const Value& value, ColumnType type) const {
  switch (type) {
    case ColumnType::BigInt:
      if (value.kind == Value::Kind::Int) return std::to_string(value.i);
      break;
    case ColumnType::Varchar:
      if (value.kind == Value::Kind::Str) return value.s;
      break;
    case ColumnType::Vector:
      if (value.kind == Value::Kind::Vec) {
        std::string out = "[";
        for (size_t i = 0; i < value.v.size(); ++i) {
          if (i != 0) out += ",";
          out += format_double(static_cast<double>(value.v[i]), "%g");
        }
        out += "]";
        return out;
      }
      break;
  }
  if (value.kind == Value::Kind::Float) return format_double(value.d, float_fmt_);
  if (value.kind == Value::Kind::Int) return std::to_string(value.i);
  if (value.kind == Value::Kind::Str) return value.s;
  return std::string();
}

ResultSet Engine::execute(const Statement& stmt) {
  switch (stmt.kind) {
    case Statement::Kind::Begin: {
      if (in_txn_) {
        throw_sql_error(kErrNotSupported, "nested transactions are not supported");
      }
      txn_snapshot_ = std::make_unique<detail::Database>(db_);
      in_txn_ = true;
      return ResultSet{};
    }
    case Statement::Kind::Commit: {
      if (in_txn_) {
        txn_snapshot_.reset();
        in_txn_ = false;
      }
      return ResultSet{};
    }
    case Statement::Kind::Rollback: {
      if (in_txn_) {
        db_ = *txn_snapshot_;
        txn_snapshot_.reset();
        in_txn_ = false;
      }
      return ResultSet{};
    }
    default:
      break;
  }

  // Statement atomicity: a failed statement must leave no partial modification.
  detail::Database backup = db_;
  try {
    switch (stmt.kind) {
      case Statement::Kind::CreateTable: return exec_create(stmt);
      case Statement::Kind::Insert: return exec_insert(stmt);
      case Statement::Kind::Select: return exec_select(stmt);
      case Statement::Kind::Update: return exec_update(stmt);
      case Statement::Kind::Delete: return exec_delete(stmt);
      default: break;
    }
  } catch (...) {
    db_ = std::move(backup);
    throw;
  }
  throw_sql_error(kErrInternal, "unhandled statement kind");
}

}  // namespace mvdb
