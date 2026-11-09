#include "exec/engine.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <set>
#include <utility>

#include "storage/catalog.h"
#include "storage/heap_file.h"
#include "storage/record.h"
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

int compare_values(const Value& a, const Value& b) {
  if (value_is_numeric(a) && value_is_numeric(b)) {
    const double x = value_as_double(a);
    const double y = value_as_double(b);
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
  }
  if (a.kind == Value::Kind::Str && b.kind == Value::Kind::Str) {
    const int cmp = a.s.compare(b.s);
    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
    return 0;
  }
  throw_sql_error(kErrNotSupported, "ORDER BY on this value type is not supported");
}

}  // namespace

Engine::Engine(std::string data_dir, size_t pool_pages)
    : data_dir_(std::move(data_dir)), pool_pages_(pool_pages) {
  if (!data_dir_.empty() && data_dir_.back() == '/') data_dir_.pop_back();
  catalog_path_ = data_dir_ + "/catalog.meta";
}

Engine::~Engine() {
  if (in_txn_) {
    rollback_to(0);
    in_txn_ = false;
  }
  for (auto& entry : tables_) {
    if (entry.second->heap) entry.second->heap->close();
  }
}

void Engine::shutdown() {
  if (in_txn_) {
    rollback_to(0);
    in_txn_ = false;
  }
  flush_all();
}

std::string Engine::heap_path(uint32_t table_id) const {
  return data_dir_ + "/t" + std::to_string(table_id) + ".heap";
}

bool Engine::open(std::string& error) {
  if (!data_dir_.empty()) {
    if (::mkdir(data_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
      error = "cannot create data directory '" + data_dir_ + "': " + std::strerror(errno);
      return false;
    }
  }
  std::vector<TableMeta> metas;
  if (!load_catalog(catalog_path_, metas, next_table_id_)) {
    error = "cannot read catalog '" + catalog_path_ + "'";
    return false;
  }
  for (TableMeta& meta : metas) {
    auto state = std::make_unique<TableState>();
    state->meta = meta;
    state->heap = std::make_unique<HeapFile>(heap_path(meta.id), pool_pages_);
    if (!state->heap->open()) {
      error = "cannot open heap file for table '" + meta.name + "'";
      return false;
    }
    // Temporary primary-key index: rebuilt by a full scan at startup. It is
    // replaced by an on-disk B+tree in milestone M1b.
    bool corrupt = false;
    state->heap->scan([&](RID rid, std::string_view bytes) {
      int64_t pk = 0;
      if (!decode_primary_key(state->meta, bytes, pk)) {
        corrupt = true;
        return;
      }
      if (!state->pk_map.emplace(pk, rid).second) corrupt = true;
    });
    if (corrupt) {
      error = "corrupt heap file for table '" + meta.name + "'";
      return false;
    }
    by_name_[lower(meta.name)] = meta.id;
    tables_[meta.id] = std::move(state);
  }
  return true;
}

Engine::TableState& Engine::require_table(const std::string& name) {
  auto it = by_name_.find(lower(name));
  if (it == by_name_.end()) {
    throw_sql_error(kErrTableNotFound, "Table '" + name + "' doesn't exist");
  }
  return *tables_.at(it->second);
}

const Engine::TableState& Engine::require_table(const std::string& name) const {
  auto it = by_name_.find(lower(name));
  if (it == by_name_.end()) {
    throw_sql_error(kErrTableNotFound, "Table '" + name + "' doesn't exist");
  }
  return *tables_.at(it->second);
}

size_t Engine::require_column(const TableMeta& meta, const std::string& name) const {
  for (size_t i = 0; i < meta.columns.size(); ++i) {
    if (meta.columns[i].name == name) return i;
  }
  for (size_t i = 0; i < meta.columns.size(); ++i) {
    if (iequals(meta.columns[i].name, name)) return i;
  }
  throw_sql_error(kErrUnknownColumn, "Unknown column '" + name + "'");
}

Value Engine::coerce(const TableMeta& meta, size_t column_index, const Value& input) const {
  const Column& col = meta.columns[column_index];
  switch (col.type) {
    case ColumnType::BigInt: {
      if (input.kind == Value::Kind::Int) return input;
      if (input.kind == Value::Kind::Str) {
        int64_t v = 0;
        if (!parse_int64(input.s, v)) {
          throw_sql_error(kErrIncorrectValue,
                          "Incorrect integer value for column '" + col.name + "'");
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
        throw_sql_error(kErrIncorrectValue,
                        "Incorrect value for VARCHAR column '" + col.name + "'");
      }
      if (!is_valid_utf8(text)) {
        throw_sql_error(kErrIncorrectValue, "Invalid UTF-8 for column '" + col.name + "'");
      }
      const size_t chars = utf8_codepoint_count(text);
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
        throw_sql_error(kErrIncorrectValue,
                        "Incorrect value for VECTOR column '" + col.name + "'");
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

bool Engine::matches(const TableMeta& meta, const std::vector<Value>& row,
                     const std::vector<Condition>& conds) const {
  for (const Condition& cond : conds) {
    const size_t idx = require_column(meta, cond.column);
    const Value& actual = row[idx];
    const Value expected = coerce(meta, idx, cond.literal);
    if (actual.kind == Value::Kind::Vec || expected.kind == Value::Kind::Vec) {
      throw_sql_error(kErrNotSupported, "comparison on VECTOR columns is not supported");
    }
    const int cmp = compare_values(actual, expected);
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
  if (by_name_.count(key) != 0) {
    throw_sql_error(kErrTableExists, "Table '" + stmt.table + "' already exists");
  }
  TableMeta meta;
  meta.id = next_table_id_;
  meta.name = stmt.table;
  std::set<std::string> seen;
  for (const ColumnDef& def : stmt.columns) {
    if (!seen.insert(lower(def.name)).second) {
      throw_sql_error(kErrDuplicateColumn, "Duplicate column name '" + def.name + "'");
    }
    Column col;
    col.name = def.name;
    col.type = def.type;
    col.length = def.length;
    col.dim = def.dim;
    if (def.primary_key) meta.pk_index = meta.columns.size();
    meta.columns.push_back(std::move(col));
  }

  auto state = std::make_unique<TableState>();
  state->meta = meta;
  state->heap = std::make_unique<HeapFile>(heap_path(meta.id), pool_pages_);
  if (!state->heap->open()) {
    throw_sql_error(kErrInternal, "cannot create storage for table '" + stmt.table + "'");
  }
  const uint32_t table_id = meta.id;
  tables_[table_id] = std::move(state);
  by_name_[key] = table_id;
  ++next_table_id_;
  if (!save_catalog()) {
    drop_table(table_id);
    throw_sql_error(kErrInternal, "cannot persist catalog");
  }
  undo_.push_back(UndoOp{UndoOp::Kind::CreatedTable, table_id, 0, 0, {}});
  return ResultSet{};
}

ResultSet Engine::exec_insert(const Statement& stmt) {
  TableState& table = require_table(stmt.table);
  std::vector<size_t> targets;
  if (stmt.insert_columns.empty()) {
    for (size_t i = 0; i < table.meta.columns.size(); ++i) targets.push_back(i);
  } else {
    std::set<size_t> seen;
    for (const std::string& name : stmt.insert_columns) {
      const size_t idx = require_column(table.meta, name);
      if (!seen.insert(idx).second) {
        throw_sql_error(kErrIncorrectValue, "Column '" + name + "' specified twice");
      }
      targets.push_back(idx);
    }
    if (targets.size() != table.meta.columns.size()) {
      throw_sql_error(kErrIncorrectValue,
                      "Column count doesn't match value count: every column must be provided");
    }
  }

  for (const std::vector<Value>& input_row : stmt.rows) {
    if (input_row.size() != targets.size()) {
      throw_sql_error(kErrIncorrectValue, "Column count doesn't match value count");
    }
    std::vector<Value> values(table.meta.columns.size());
    for (size_t i = 0; i < targets.size(); ++i) {
      values[targets[i]] = coerce(table.meta, targets[i], input_row[i]);
    }
    const int64_t pk = values[table.meta.pk_index].i;
    if (table.pk_map.count(pk) != 0) {
      throw_sql_error(kErrDuplicateKey, "Duplicate entry '" + std::to_string(pk) +
                                            "' for key 'PRIMARY'");
    }
    const std::string encoded = encode_row(table.meta, values);
    const RID rid = table.heap->insert(encoded);
    if (rid == kInvalidRid) {
      throw_sql_error(kErrInternal, "cannot allocate space for row");
    }
    table.pk_map.emplace(pk, rid);
    undo_.push_back(UndoOp{UndoOp::Kind::InsertedRow, table.meta.id, pk, rid, {}});
  }
  ResultSet rs;
  rs.affected_rows = static_cast<int64_t>(stmt.rows.size());
  return rs;
}

ResultSet Engine::exec_delete(const Statement& stmt) {
  TableState& table = require_table(stmt.table);
  struct Victim {
    RID rid;
    int64_t pk;
    std::string record;
  };
  std::vector<Victim> victims;
  table.heap->scan([&](RID rid, std::string_view bytes) {
    std::vector<Value> row;
    if (!decode_row(table.meta, bytes, row)) return;
    if (stmt.has_where && !matches(table.meta, row, stmt.where)) return;
    victims.push_back(Victim{rid, row[table.meta.pk_index].i, std::string(bytes)});
  });

  for (const Victim& victim : victims) {
    table.heap->erase(victim.rid);
    table.pk_map.erase(victim.pk);
    undo_.push_back(UndoOp{UndoOp::Kind::DeletedRow, table.meta.id, victim.pk, 0, victim.record});
  }
  ResultSet rs;
  rs.affected_rows = static_cast<int64_t>(victims.size());
  return rs;
}

ResultSet Engine::exec_update(const Statement& stmt) {
  TableState& table = require_table(stmt.table);
  struct Assignment {
    size_t column;
    Value value;
  };
  std::vector<Assignment> assignments;
  for (const auto& item : stmt.set_items) {
    const size_t idx = require_column(table.meta, item.first);
    if (idx == table.meta.pk_index) {
      throw_sql_error(kErrNotSupported, "updating the primary key is not supported");
    }
    assignments.push_back(Assignment{idx, coerce(table.meta, idx, item.second)});
  }

  struct Target {
    RID rid;
    int64_t pk;
    std::string old_record;
    std::vector<Value> new_values;
  };
  std::vector<Target> targets;
  table.heap->scan([&](RID rid, std::string_view bytes) {
    std::vector<Value> row;
    if (!decode_row(table.meta, bytes, row)) return;
    if (stmt.has_where && !matches(table.meta, row, stmt.where)) return;
    Target target;
    target.rid = rid;
    target.pk = row[table.meta.pk_index].i;
    target.old_record.assign(bytes.data(), bytes.size());
    for (const Assignment& assignment : assignments) {
      row[assignment.column] = assignment.value;
    }
    target.new_values = std::move(row);
    targets.push_back(std::move(target));
  });

  for (const Target& target : targets) {
    const std::string encoded = encode_row(table.meta, target.new_values);
    const RID new_rid = table.heap->update(target.rid, encoded);
    if (new_rid == kInvalidRid) {
      throw_sql_error(kErrInternal, "cannot allocate space for updated row");
    }
    table.pk_map[target.pk] = new_rid;
    undo_.push_back(
        UndoOp{UndoOp::Kind::UpdatedRow, table.meta.id, target.pk, new_rid, target.old_record});
  }
  ResultSet rs;
  rs.affected_rows = static_cast<int64_t>(targets.size());
  return rs;
}

ResultSet Engine::exec_select(const Statement& stmt) {
  TableState& table = require_table(stmt.table);
  const TableMeta& meta = table.meta;

  struct OutputSpec {
    size_t table_index = 0;
    bool is_distance = false;
    std::vector<float> query;
    std::string name;
  };
  std::vector<OutputSpec> specs;
  if (stmt.select_star) {
    for (size_t i = 0; i < meta.columns.size(); ++i) {
      OutputSpec spec;
      spec.table_index = i;
      spec.name = meta.columns[i].name;
      specs.push_back(std::move(spec));
    }
  } else {
    for (const SelectItem& item : stmt.select_items) {
      OutputSpec spec;
      spec.table_index = require_column(meta, item.column);
      spec.is_distance = item.is_distance;
      spec.query = item.query;
      spec.name = item.alias;
      if (spec.name.empty()) {
        spec.name = item.is_distance ? "distance" : meta.columns[spec.table_index].name;
      }
      if (item.is_distance) {
        if (meta.columns[spec.table_index].type != ColumnType::Vector) {
          throw_sql_error(kErrIncorrectValue, "L2_DISTANCE requires a VECTOR column");
        }
        if (static_cast<int64_t>(item.query.size()) != meta.columns[spec.table_index].dim) {
          throw_sql_error(kErrIncorrectValue, "L2_DISTANCE query vector dimension mismatch");
        }
      }
      specs.push_back(std::move(spec));
    }
  }

  struct SortKeySpec {
    int out_index = -1;
    int extra_index = -1;
    bool asc = true;
  };
  std::vector<SortKeySpec> sort_keys;
  std::vector<size_t> extra_columns;
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
      const size_t column = require_column(meta, key.name);
      auto it = std::find(extra_columns.begin(), extra_columns.end(), column);
      if (it == extra_columns.end()) {
        extra_columns.push_back(column);
        spec.extra_index = static_cast<int>(extra_columns.size() - 1);
      } else {
        spec.extra_index = static_cast<int>(it - extra_columns.begin());
      }
    }
    sort_keys.push_back(spec);
  }

  struct OutRow {
    std::vector<Value> raw;
    std::vector<Value> extras;
    int64_t pk = 0;
  };
  std::vector<OutRow> out_rows;
  table.heap->scan([&](RID rid, std::string_view bytes) {
    std::vector<Value> row;
    if (!decode_row(meta, bytes, row)) return;
    if (stmt.has_where && !matches(meta, row, stmt.where)) return;
    OutRow out;
    out.pk = row[meta.pk_index].i;
    out.raw.reserve(specs.size());
    for (const OutputSpec& spec : specs) {
      if (spec.is_distance) {
        Value v;
        v.kind = Value::Kind::Float;
        v.d = l2_distance(row[spec.table_index].v, spec.query);
        out.raw.push_back(std::move(v));
      } else {
        out.raw.push_back(row[spec.table_index]);
      }
    }
    for (size_t column : extra_columns) out.extras.push_back(row[column]);
    out_rows.push_back(std::move(out));
  });

  if (!sort_keys.empty()) {
    std::stable_sort(out_rows.begin(), out_rows.end(), [&](const OutRow& a, const OutRow& b) {
      for (const SortKeySpec& key : sort_keys) {
        const Value& va = key.out_index >= 0 ? a.raw[static_cast<size_t>(key.out_index)]
                                             : a.extras[static_cast<size_t>(key.extra_index)];
        const Value& vb = key.out_index >= 0 ? b.raw[static_cast<size_t>(key.out_index)]
                                             : b.extras[static_cast<size_t>(key.extra_index)];
        int cmp = compare_values(va, vb);
        if (!key.asc) cmp = -cmp;
        if (cmp != 0) return cmp < 0;
      }
      return a.pk < b.pk;
    });
  }

  if (stmt.has_limit) {
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
      line.push_back(format_value(out.raw[i], meta.columns[specs[i].table_index].type));
    }
    rs.rows.push_back(std::move(line));
  }
  return rs;
}

double Engine::l2_distance(const std::vector<float>& a, const std::vector<float>& b) const {
  double sum = 0.0;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
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

void Engine::drop_table(uint32_t table_id) {
  auto it = tables_.find(table_id);
  if (it == tables_.end()) return;
  by_name_.erase(lower(it->second->meta.name));
  if (it->second->heap) {
    it->second->heap->close();
    ::unlink(heap_path(table_id).c_str());
  }
  tables_.erase(it);
}

void Engine::rollback_to(size_t mark) {
  for (size_t i = undo_.size(); i > mark; --i) {
    const UndoOp& op = undo_[i - 1];
    auto it = tables_.find(op.table_id);
    if (it == tables_.end()) continue;
    TableState& table = *it->second;
    switch (op.kind) {
      case UndoOp::Kind::InsertedRow:
        table.heap->erase(op.rid);
        table.pk_map.erase(op.pk);
        break;
      case UndoOp::Kind::DeletedRow:
      case UndoOp::Kind::UpdatedRow: {
        if (op.kind == UndoOp::Kind::UpdatedRow) table.heap->erase(op.rid);
        const RID restored = table.heap->insert(op.record);
        if (restored != kInvalidRid) table.pk_map[op.pk] = restored;
        break;
      }
      case UndoOp::Kind::CreatedTable:
        drop_table(op.table_id);
        break;
    }
  }
  undo_.resize(mark);
  save_catalog();
}

bool Engine::flush_all() {
  bool ok = true;
  for (auto& entry : tables_) {
    if (entry.second->heap && !entry.second->heap->sync()) ok = false;
  }
  return ok;
}

bool Engine::save_catalog() {
  std::vector<TableMeta> metas;
  metas.reserve(tables_.size());
  for (const auto& entry : tables_) metas.push_back(entry.second->meta);
  std::sort(metas.begin(), metas.end(),
            [](const TableMeta& a, const TableMeta& b) { return a.id < b.id; });
  return ::mvdb::save_catalog(catalog_path_, metas, next_table_id_);
}

ResultSet Engine::execute(const Statement& stmt) {
  switch (stmt.kind) {
    case Statement::Kind::Begin:
      if (in_txn_) throw_sql_error(kErrNotSupported, "nested transactions are not supported");
      in_txn_ = true;
      undo_.clear();
      return ResultSet{};
    case Statement::Kind::Commit:
      if (in_txn_) {
        flush_all();
        undo_.clear();
        in_txn_ = false;
      }
      return ResultSet{};
    case Statement::Kind::Rollback:
      if (in_txn_) {
        rollback_to(0);
        flush_all();
        in_txn_ = false;
      }
      return ResultSet{};
    default:
      break;
  }

  const size_t mark = undo_.size();
  try {
    ResultSet rs;
    switch (stmt.kind) {
      case Statement::Kind::CreateTable: rs = exec_create(stmt); break;
      case Statement::Kind::Insert: rs = exec_insert(stmt); break;
      case Statement::Kind::Select: rs = exec_select(stmt); break;
      case Statement::Kind::Update: rs = exec_update(stmt); break;
      case Statement::Kind::Delete: rs = exec_delete(stmt); break;
      default: throw_sql_error(kErrInternal, "unhandled statement kind");
    }
    if (!in_txn_) {
      flush_all();
      undo_.clear();
    }
    return rs;
  } catch (...) {
    rollback_to(mark);
    if (!in_txn_) flush_all();
    throw;
  }
}

}  // namespace mvdb
