#include "exec/engine.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <utility>

#include "storage/btree.h"
#include "storage/catalog.h"
#include "storage/heap_file.h"
#include "storage/record.h"
#include "vector/distance.h"
#include "vector/hnsw.h"
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
  wal_path_ = data_dir_ + "/wal.log";
  if (const char* value = std::getenv("MVDB_EF_FLOOR")) ef_floor_ = std::strtoul(value, nullptr, 10);
  if (const char* value = std::getenv("MVDB_EF_MAX")) ef_max_ = std::strtoul(value, nullptr, 10);
}

Engine::~Engine() {
  if (in_txn_) {
    rollback_to(0);
    in_txn_ = false;
  }
  for (auto& entry : tables_) {
    if (entry.second->heap) entry.second->heap->close();
    if (entry.second->pk_index) entry.second->pk_index->close();
  }
  wal_.close();
}

void Engine::shutdown() {
  if (in_txn_) {
    rollback_to(0);
    release_current_txn();
    in_txn_ = false;
  }
  checkpoint();
}

std::string Engine::heap_path(uint32_t table_id) const {
  return data_dir_ + "/t" + std::to_string(table_id) + ".heap";
}

std::string Engine::btree_path(uint32_t table_id) const {
  return data_dir_ + "/t" + std::to_string(table_id) + ".btree";
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
  if (!wal_.open(wal_path_, error)) return false;
  // Replay happens against raw file descriptors, before any heap or index is
  // opened, so no cached page can shadow a replayed image.
  if (!recover(metas, error)) return false;
  for (TableMeta& meta : metas) {
    auto state = std::make_unique<TableState>();
    state->meta = meta;
    state->heap = std::make_unique<HeapFile>(heap_path(meta.id), pool_pages_);
    if (!state->heap->open()) {
      error = "cannot open heap file for table '" + meta.name + "'";
      return false;
    }
    state->pk_index = std::make_unique<BTree>(btree_path(meta.id), pool_pages_);
    if (!state->pk_index->open()) {
      error = "cannot open primary-key index for table '" + meta.name + "'";
      return false;
    }
    if (!state->pk_index->built()) {
      // A crash during a previous build leaves the flag unset, so rebuilding is
      // idempotent: insert() upserts, and the flag is only set after success.
      bool corrupt = false;
      state->heap->scan([&](RID rid, std::string_view bytes) {
        int64_t pk = 0;
        if (!decode_primary_key(state->meta, bytes, pk)) {
          corrupt = true;
          return;
        }
        state->pk_index->insert(pk, rid);
      });
      if (corrupt) {
        error = "corrupt heap file for table '" + meta.name + "'";
        return false;
      }
      state->pk_index->mark_built();
    }
    // The vector graph is derived state and is built on demand (first
    // approximate query), so opening a database never pays for it.
    by_name_[lower(meta.name)] = meta.id;
    tables_[meta.id] = std::move(state);
  }
  return true;
}

ResultSet Engine::exec_drop_table(const Statement& stmt) {
  auto it = by_name_.find(lower(stmt.table));
  if (it == by_name_.end()) {
    if (stmt.if_exists) return ResultSet{};
    throw_sql_error(kErrTableNotFound, "Unknown table '" + stmt.table + "'");
  }
  if (in_txn_) {
    throw_sql_error(kErrNotSupported, "DROP TABLE is not supported inside a transaction");
  }
  drop_table(it->second);
  if (!save_catalog()) throw_sql_error(kErrInternal, "cannot persist catalog");
  return ResultSet{};
}

ResultSet Engine::exec_create_vector_index(const Statement& stmt) {
  TableState& table = require_table(stmt.table);
  const size_t column = require_column(table.meta, stmt.index_column);
  if (table.meta.columns[column].type != ColumnType::Vector) {
    throw_sql_error(kErrIncorrectValue, "vector index requires a VECTOR column");
  }
  if (table.meta.has_vector_index) {
    throw_sql_error(kErrNotSupported, "the table already has a vector index");
  }
  table.meta.has_vector_index = true;
  table.meta.vector_column = static_cast<int32_t>(column);
  build_vector_index(table);
  if (!save_catalog()) {
    table.meta.has_vector_index = false;
    table.meta.vector_column = -1;
    table.vec_index.reset();
    throw_sql_error(kErrInternal, "cannot persist catalog");
  }
  undo_.push_back(UndoOp{UndoOp::Kind::CreatedVectorIndex, table.meta.id, 0, 0, {}});
  return ResultSet{};
}

ResultSet Engine::exec_set_mode(const Statement& stmt) {
  const std::string value = to_lower(stmt.mode_value);
  if (value == "exact") {
    mode_ = QueryMode::Exact;
  } else if (value == "ann") {
    mode_ = QueryMode::Ann;
  } else {
    throw_sql_error(kErrIncorrectValue, "mode must be 'exact' or 'ann'");
  }
  return ResultSet{};
}

void Engine::build_vector_index(TableState& table) {
  const auto started = std::chrono::steady_clock::now();
  const TableMeta& meta = table.meta;
  if (meta.vector_column < 0 || static_cast<size_t>(meta.vector_column) >= meta.columns.size()) {
    throw_sql_error(kErrInternal, "vector index refers to a missing column");
  }
  const int64_t dim = meta.columns[static_cast<size_t>(meta.vector_column)].dim;
  // M=32: on uniform random 128-dim data M=16 tops out around 0.87 recall even
  // at ef=256, while M=32 reaches ~0.97 for the same search budget.
  table.vec_index = std::make_unique<VectorIndex>(dim, 32, 200,
                                                  static_cast<uint64_t>(meta.id) * 7919 + 13);
  table.heap->scan([&](RID rid, std::string_view bytes) {
    std::vector<Value> row;
    if (!decode_row(meta, bytes, row)) return;
    const int64_t pk = row[meta.pk_index].i;
    // Copy the vector before moving the row: argument evaluation order is
    // unspecified, and moving first would leave this reference dangling.
    const std::vector<float> vec = row[static_cast<size_t>(meta.vector_column)].v;
    table.vec_index->add(pk, vec, std::move(row));
  });
  table.vec_dirty = false;
  timings_.index_build_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
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
        throw_sql_error(kErrVarcharTooLong,
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
        throw_sql_error(kErrVectorDimension,
                        "Invalid vector dimension for column '" + col.name + "': expected " +
                            std::to_string(col.dim) + ", got " + std::to_string(vec.size()));
      }
      return Value::make_vec(std::move(vec));
    }
  }
  throw_sql_error(kErrInternal, "unreachable coercion path");
}

struct PreparedCondition {
  size_t column;
  Condition::Op op;
  Value value;
};

std::vector<PreparedCondition> Engine::prepare_filter(const TableMeta& meta,
                                                      const std::vector<Condition>& conds) const {
  std::vector<PreparedCondition> out;
  out.reserve(conds.size());
  for (const Condition& cond : conds) {
    const size_t idx = require_column(meta, cond.column);
    out.push_back(PreparedCondition{idx, cond.op, coerce(meta, idx, cond.literal)});
  }
  return out;
}

bool Engine::matches_prepared(const std::vector<PreparedCondition>& prepared,
                              const std::vector<Value>& row) const {
  for (const PreparedCondition& cond : prepared) {
    const Value& actual = row[cond.column];
    const int cmp = compare_values(actual, cond.value);
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

bool Engine::locate_raw_row(const TableMeta& meta, std::string_view bytes,
                            const std::vector<PreparedCondition>& prepared, int vector_column,
                            RawRow& out) const {
  size_t pos = 0;
  out.filter_columns.assign(prepared.size(), nullptr);
  for (size_t i = 0; i < meta.columns.size(); ++i) {
    const Column& column = meta.columns[i];
    switch (column.type) {
      case ColumnType::BigInt: {
        if (pos + sizeof(int64_t) > bytes.size()) return false;
        const char* at = bytes.data() + pos;
        if (i == meta.pk_index) out.pk = at;
        for (size_t f = 0; f < prepared.size(); ++f) {
          if (prepared[f].column == i) out.filter_columns[f] = at;
        }
        pos += sizeof(int64_t);
        break;
      }
      case ColumnType::Varchar: {
        if (pos + sizeof(uint32_t) > bytes.size()) return false;
        uint32_t len = 0;
        std::memcpy(&len, bytes.data() + pos, sizeof(len));
        pos += sizeof(uint32_t);
        if (pos + len > bytes.size()) return false;
        const char* at = bytes.data() + pos;
        for (size_t f = 0; f < prepared.size(); ++f) {
          if (prepared[f].column == i) out.filter_columns[f] = at;
        }
        pos += len;
        break;
      }
      case ColumnType::Vector: {
        const size_t bytes_needed = static_cast<size_t>(column.dim) * sizeof(float);
        if (pos + bytes_needed > bytes.size()) return false;
        if (vector_column == static_cast<int>(i)) {
          out.vec = reinterpret_cast<const float*>(bytes.data() + pos);
        }
        for (size_t f = 0; f < prepared.size(); ++f) {
          if (prepared[f].column == i) return false;  // vector comparison is rejected earlier
        }
        pos += bytes_needed;
        break;
      }
    }
  }
  return pos == bytes.size();
}

bool Engine::raw_row_matches(const std::vector<PreparedCondition>& prepared,
                             const RawRow& raw) const {
  for (size_t f = 0; f < prepared.size(); ++f) {
    const PreparedCondition& cond = prepared[f];
    const char* at = raw.filter_columns[f];
    if (at == nullptr) return false;
    int cmp = 0;
    if (cond.value.kind == Value::Kind::Int) {
      int64_t stored = 0;
      std::memcpy(&stored, at, sizeof(stored));
      if (stored < cond.value.i) {
        cmp = -1;
      } else if (stored > cond.value.i) {
        cmp = 1;
      }
    } else {
      const uint32_t len = *reinterpret_cast<const uint32_t*>(at - sizeof(uint32_t));
      const std::string& expected = cond.value.s;
      const size_t common = std::min<size_t>(len, expected.size());
      cmp = std::memcmp(at, expected.data(), common);
      if (cmp == 0) {
        if (len < expected.size()) {
          cmp = -1;
        } else if (len > expected.size()) {
          cmp = 1;
        }
      }
    }
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
    if (stmt.if_not_exists) return ResultSet{};
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
  state->pk_index = std::make_unique<BTree>(btree_path(meta.id), pool_pages_);
  if (!state->pk_index->open() || !state->pk_index->mark_built()) {
    throw_sql_error(kErrInternal, "cannot create index for table '" + stmt.table + "'");
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
    RID existing = 0;
    if (table.pk_index->search(pk, existing)) {
      throw_sql_error(kErrDuplicateKey, "Duplicate entry '" + std::to_string(pk) +
                                            "' for key 'PRIMARY'");
    }
    const std::string encoded = encode_row(table.meta, values);
    const RID rid = table.heap->insert(encoded);
    if (rid == kInvalidRid) {
      throw_sql_error(kErrInternal, "cannot allocate space for row");
    }
    table.pk_index->insert(pk, rid);
    if (table.vec_index) {
      table.vec_index->add(pk, values[static_cast<size_t>(table.meta.vector_column)].v, values);
    }
    undo_.push_back(UndoOp{UndoOp::Kind::InsertedRow, table.meta.id, pk, rid, {}});
  }
  ResultSet rs;
  rs.has_affected_rows = true;
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
    table.pk_index->erase(victim.pk);
    if (table.vec_index) table.vec_index->remove(victim.pk);
    undo_.push_back(UndoOp{UndoOp::Kind::DeletedRow, table.meta.id, victim.pk, 0, victim.record});
  }
  ResultSet rs;
  rs.has_affected_rows = true;
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
    table.pk_index->insert(target.pk, new_rid);
    if (table.vec_index) {
      table.vec_index->remove(target.pk);
      table.vec_index->add(target.pk,
                           target.new_values[static_cast<size_t>(table.meta.vector_column)].v,
                           target.new_values);
    }
    undo_.push_back(
        UndoOp{UndoOp::Kind::UpdatedRow, table.meta.id, target.pk, new_rid, target.old_record});
  }
  ResultSet rs;
  rs.has_affected_rows = true;
  rs.affected_rows = static_cast<int64_t>(targets.size());
  return rs;
}

ResultSet Engine::exec_select(const Statement& stmt) {
  if (stmt.no_from) {
    ResultSet rs;
    rs.has_rows = true;
    std::vector<std::string> row;
    for (const SelectItem& item : stmt.select_items) {
      rs.columns.push_back(item.alias.empty() ? item.literal_text : item.alias);
      switch (item.literal.kind) {
        case Value::Kind::Int: row.push_back(std::to_string(item.literal.i)); break;
        case Value::Kind::Float: row.push_back(format_double(item.literal.d, float_fmt_)); break;
        case Value::Kind::Str: row.push_back(item.literal.s); break;
        case Value::Kind::Vec: row.push_back(format_value(item.literal, ColumnType::Vector)); break;
        default: row.push_back(std::string()); break;
      }
    }
    rs.rows.push_back(std::move(row));
    return rs;
  }
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
          throw_sql_error(kErrNotSupported, "L2_DISTANCE requires a VECTOR column");
        }
        if (static_cast<int64_t>(item.query.size()) != meta.columns[spec.table_index].dim) {
          throw_sql_error(kErrVectorDimension, "L2_DISTANCE query vector dimension mismatch");
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

  const std::vector<PreparedCondition> prepared =
      stmt.has_where ? prepare_filter(meta, stmt.where) : std::vector<PreparedCondition>{};

  struct OutRow {
    std::vector<Value> raw;
    std::vector<Value> extras;
    int64_t pk = 0;
  };
  std::vector<OutRow> out_rows;

  int dist_slot = -1;
  for (size_t i = 0; i < specs.size(); ++i) {
    if (!specs[i].is_distance) continue;
    if (dist_slot >= 0) {
      dist_slot = -2;
      break;
    }
    dist_slot = static_cast<int>(i);
  }
  // Eligibility depends on the index *definition*, not on whether the graph is
  // already materialized: the graph is built lazily on first use.
  const bool ann_eligible =
      mode_ == QueryMode::Ann && table.meta.has_vector_index &&
      table.meta.vector_column >= 0 && stmt.has_limit && stmt.limit > 0 &&
      dist_slot >= 0 && sort_keys.size() == 1 && sort_keys[0].out_index == dist_slot &&
      sort_keys[0].asc;
  bool used_ann = false;
  if (ann_eligible) {
    const auto ann_started = std::chrono::steady_clock::now();
    struct AnnTimer {
      Engine::Timings* timings;
      std::chrono::steady_clock::time_point started;
      ~AnnTimer() {
        timings->select_ann_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
      }
    } ann_timer{&timings_, ann_started};
    ++timings_.ann_queries;
    if (!table.vec_index || table.vec_dirty) build_vector_index(table);
    const size_t k = static_cast<size_t>(stmt.limit);
    const std::vector<float>& query = specs[static_cast<size_t>(dist_slot)].query;
    const VectorIndex::AcceptFn accept = [&](const std::vector<Value>& row) {
      return prepared.empty() || matches_prepared(prepared, row);
    };
    std::vector<VectorIndex::Hit> hits;
    size_t ef = ef_floor_;
    const bool debug_ann = std::getenv("MVDB_DEBUG_ANN") != nullptr;
    while (true) {
      VectorIndex::SearchStats stats;
      hits = table.vec_index->search(query, k, ef, accept, &stats);
      if (debug_ann) {
        std::fprintf(stderr, "[ann] ef=%zu hits=%zu visited=%zu accepted=%zu\n", ef, hits.size(),
                     stats.visited, stats.accepted);
      }
      // "Enough results" is not enough. A filtered query must widen the search
      // by roughly 1/selectivity to see as many matching nodes as an
      // unfiltered query would; otherwise the accepted set is too shallow and
      // recall silently collapses.
      if (hits.size() < k) {
        if (ef >= ef_max_) break;
        ef *= 2;
        continue;
      }
      const double selectivity = stats.visited == 0
                                     ? 1.0
                                     : static_cast<double>(stats.accepted) / stats.visited;
      const size_t needed = selectivity < 0.01
                                ? ef_max_
                                : static_cast<size_t>(static_cast<double>(ef_floor_) / selectivity);
      const size_t target = std::min(std::max(ef, needed), ef_max_);
      if (ef >= target) break;
      ef = target;
    }
    timings_.ann_ef_total += ef;
    if (hits.size() >= k) {
      for (const VectorIndex::Hit& hit : hits) {
        RID rid = 0;
        if (!table.pk_index->search(hit.pk, rid)) continue;
        std::string bytes;
        if (!table.heap->read(rid, bytes)) continue;
        std::vector<Value> row;
        if (!decode_row(meta, bytes, row)) continue;
        OutRow out;
        out.pk = hit.pk;
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
      }
      used_ann = true;
    }
  }
  const auto scan_started = std::chrono::steady_clock::now();
  // Allocation-free Top-K: walk the encoded rows, keep a bounded heap, and only
  // decode the k rows that survive. The general path below decodes every row,
  // which at 10k x 128 costs more than the distance arithmetic itself.
  const bool topk_shape = dist_slot >= 0 && stmt.has_limit && stmt.limit > 0 &&
                          sort_keys.size() == 1 && sort_keys[0].out_index == dist_slot &&
                          sort_keys[0].asc;
  bool used_fast = false;
  if (!used_ann && topk_shape) {
    const size_t limit = static_cast<size_t>(stmt.limit);
    const int vec_column = static_cast<int>(specs[static_cast<size_t>(dist_slot)].table_index);
    const std::vector<float>& query = specs[static_cast<size_t>(dist_slot)].query;
    const size_t dim = query.size();
    std::vector<std::pair<double, int64_t>> top;  // max-heap: worst kept entry on top
    top.reserve(limit + 1);
    table.heap->scan([&](RID rid, std::string_view bytes) {
      ++timings_.rows_scanned;
      RawRow raw;
      if (!locate_raw_row(meta, bytes, prepared, vec_column, raw)) return;
      if (raw.vec == nullptr || raw.pk == nullptr) return;
      if (!prepared.empty() && !raw_row_matches(prepared, raw)) return;
      const double distance = l2_exact(raw.vec, query.data(), dim);
      int64_t pk = 0;
      std::memcpy(&pk, raw.pk, sizeof(pk));
      const auto entry = std::make_pair(distance, pk);
      if (top.size() < limit) {
        top.push_back(entry);
        std::push_heap(top.begin(), top.end());
      } else if (std::make_pair(distance, pk) < top.front()) {
        std::pop_heap(top.begin(), top.end());
        top.back() = entry;
        std::push_heap(top.begin(), top.end());
      }
    });
    std::sort(top.begin(), top.end());
    for (const auto& [distance, pk] : top) {
      RID rid = 0;
      if (!table.pk_index->search(pk, rid)) continue;
      std::string bytes;
      if (!table.heap->read(rid, bytes)) continue;
      std::vector<Value> row;
      if (!decode_row(meta, bytes, row)) continue;
      OutRow out;
      out.pk = pk;
      for (const OutputSpec& spec : specs) {
        if (spec.is_distance) {
          Value v;
          v.kind = Value::Kind::Float;
          v.d = distance;
          out.raw.push_back(std::move(v));
        } else {
          out.raw.push_back(row[spec.table_index]);
        }
      }
      for (size_t column : extra_columns) out.extras.push_back(row[column]);
      out_rows.push_back(std::move(out));
    }
    used_fast = true;
    timings_.select_scan_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scan_started)
            .count();
  }

  if (!used_ann && !used_fast) table.heap->scan([&](RID rid, std::string_view bytes) {
    ++timings_.rows_scanned;
    std::vector<Value> row;
    if (!decode_row(meta, bytes, row)) return;
    if (!prepared.empty() && !matches_prepared(prepared, row)) return;
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
  if (!used_ann && !used_fast) {
    timings_.select_scan_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scan_started)
            .count();
  }
  ++timings_.queries;

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
  for (const OutputSpec& spec : specs) {
    rs.columns.push_back(spec.name);
    if (spec.is_distance) {
      rs.column_kinds.push_back(CellKind::Float);
    } else {
      const ColumnType type = meta.columns[spec.table_index].type;
      rs.column_kinds.push_back(type == ColumnType::BigInt ? CellKind::Int : CellKind::Str);
    }
  }
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
  const size_t n = std::min(a.size(), b.size());
  return l2_exact(a.data(), b.data(), n);
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
  if (it->second->pk_index) {
    it->second->pk_index->close();
    ::unlink(btree_path(table_id).c_str());
  }
  tables_.erase(it);
}

void Engine::rollback_to(size_t mark) {
  for (size_t i = undo_.size(); i > mark; --i) {
    const UndoOp& op = undo_[i - 1];
    auto it = tables_.find(op.table_id);
    if (it == tables_.end()) continue;
    TableState& table = *it->second;
    if (op.kind == UndoOp::Kind::CreatedVectorIndex) {
      table.meta.has_vector_index = false;
      table.meta.vector_column = -1;
      table.vec_index.reset();
      table.vec_dirty = false;
      continue;
    }
    if (table.vec_index) table.vec_dirty = true;
    switch (op.kind) {
      case UndoOp::Kind::InsertedRow:
        table.heap->erase(op.rid);
        table.pk_index->erase(op.pk);
        break;
      case UndoOp::Kind::DeletedRow:
      case UndoOp::Kind::UpdatedRow: {
        if (op.kind == UndoOp::Kind::UpdatedRow) table.heap->erase(op.rid);
        const RID restored = table.heap->insert(op.record);
        if (restored != kInvalidRid) table.pk_index->insert(op.pk, restored);
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

bool Engine::checkpoint() {
  if (!flush_all()) return false;
  wal_.log_checkpoint(wal_.allocate_lsn());
  if (!wal_.sync()) return false;
  if (!wal_.truncate()) return false;
  wal_bytes_since_checkpoint_ = 0;
  return true;
}

bool Engine::flush_all() {
  bool ok = true;
  for (auto& entry : tables_) {
    if (entry.second->heap && !entry.second->heap->sync()) ok = false;
    if (entry.second->pk_index && !entry.second->pk_index->sync()) ok = false;
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

void Engine::begin_txn_scope() {
  current_txn_ = next_txid_++;
  for (auto& entry : tables_) {
    if (entry.second->heap) entry.second->heap->set_current_txn(current_txn_);
    if (entry.second->pk_index) entry.second->pk_index->set_current_txn(current_txn_);
  }
}

void Engine::release_current_txn() {
  if (current_txn_ == 0) return;
  for (auto& entry : tables_) {
    if (entry.second->heap) entry.second->heap->release_txn(current_txn_);
    if (entry.second->pk_index) entry.second->pk_index->release_txn(current_txn_);
  }
  current_txn_ = 0;
}

void Engine::commit_txn() {
  if (current_txn_ == 0) return;
  const auto started = std::chrono::steady_clock::now();
  std::vector<std::pair<uint32_t, BufferPool::DirtyPage>> pages;
  for (auto& entry : tables_) {
    const uint32_t table_id = entry.first;
    if (entry.second->heap) {
      for (auto& dirty : entry.second->heap->collect_dirty(current_txn_)) {
        pages.emplace_back(table_id * 2, dirty);
      }
    }
    if (entry.second->pk_index) {
      for (auto& dirty : entry.second->pk_index->collect_dirty(current_txn_)) {
        pages.emplace_back(table_id * 2 + 1, dirty);
      }
    }
  }
  bool ok = true;
  for (auto& item : pages) {
    const uint64_t lsn = wal_.allocate_lsn();
    item.second.page->set_lsn(lsn);
    ok = ok && wal_.log_page_image(current_txn_, item.first, item.second.page_id, lsn,
                                   item.second.page->raw(), Page::kPageSize);
    if (!ok) break;
  }
  ok = ok && wal_.log_commit(current_txn_) && wal_.sync();
  release_current_txn();
  const size_t written = pages.size() * (Page::kPageSize + 40) + 40;
  timings_.wal_bytes += written;
  wal_bytes_since_checkpoint_ += written;
  timings_.commit_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  if (!ok) throw_sql_error(kErrInternal, "write-ahead log failure");
  // Bound log growth: once the log is large, make every page durable and reset
  // it. Cheap relative to the number of commits that trigger it.
  if (wal_bytes_since_checkpoint_ >= checkpoint_threshold_) checkpoint();
}

namespace {

bool pwrite_all(int fd, const char* data, size_t len, off_t offset) {
  size_t written = 0;
  while (written < len) {
    const ssize_t n = ::pwrite(fd, data + written, len - written, offset + static_cast<off_t>(written));
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

bool Engine::recover(const std::vector<TableMeta>& metas, std::string& error) {
  std::set<uint64_t> committed;
  if (!wal_.scan([&](const Wal::Record& record) {
        if (record.type == Wal::Type::kCommit) committed.insert(record.txid);
        return true;
      })) {
    error = "cannot read write-ahead log";
    return false;
  }
  if (committed.empty()) {
    // Nothing was committed past the last checkpoint: data files are already
    // consistent because uncommitted pages are never written.
    wal_.truncate();
    return true;
  }

  std::set<uint32_t> known_tables;
  for (const TableMeta& meta : metas) known_tables.insert(meta.id);
  std::map<uint32_t, int> fds;  // file tag -> descriptor, -1 after a failure

  bool ok = true;
  wal_.scan([&](const Wal::Record& record) {
    if (!ok) return false;
    if (record.type != Wal::Type::kPageImage) return true;
    if (committed.count(record.txid) == 0) return true;
    const uint32_t table_id = record.file_tag / 2;
    if (known_tables.count(table_id) == 0) return true;  // dropped table
    auto it = fds.find(record.file_tag);
    if (it == fds.end()) {
      const bool is_index = (record.file_tag % 2) == 1;
      const std::string path = is_index ? btree_path(table_id) : heap_path(table_id);
      const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
      it = fds.emplace(record.file_tag, fd).first;
    }
    const int fd = it->second;
    if (fd < 0) return true;
    const off_t offset = static_cast<off_t>(record.page_id) * static_cast<off_t>(Page::kPageSize);
    uint64_t on_disk_lsn = 0;
    if (::pread(fd, &on_disk_lsn, sizeof(on_disk_lsn), offset + 8) ==
        static_cast<ssize_t>(sizeof(on_disk_lsn))) {
      if (on_disk_lsn >= record.lsn) return true;  // idempotent replay
    }
    if (!pwrite_all(fd, record.payload, record.payload_len, offset)) {
      ok = false;
      return false;
    }
    return true;
  });

  for (auto& entry : fds) {
    if (entry.second >= 0) {
      ::fdatasync(entry.second);
      ::close(entry.second);
    }
  }
  if (!ok) {
    error = "cannot apply write-ahead log";
    return false;
  }
  wal_.truncate();
  return true;
}

ResultSet Engine::execute(const Statement& stmt) {
  switch (stmt.kind) {
    case Statement::Kind::Begin:
      if (in_txn_) throw_sql_error(kErrNotSupported, "nested transactions are not supported");
      in_txn_ = true;
      undo_.clear();
      begin_txn_scope();
      return ResultSet{};
    case Statement::Kind::Commit:
      if (in_txn_) {
        commit_txn();
        undo_.clear();
        in_txn_ = false;
      }
      return ResultSet{};
    case Statement::Kind::Rollback:
      if (in_txn_) {
        rollback_to(0);
        release_current_txn();
        undo_.clear();
        in_txn_ = false;
      }
      return ResultSet{};
    default:
      break;
  }

  if (stmt.kind == Statement::Kind::SetMode) return exec_set_mode(stmt);
  if (stmt.kind == Statement::Kind::NoOp) return ResultSet{};
  // Reads never need a log record; they run directly in the current snapshot.
  if (stmt.kind == Statement::Kind::Select) return exec_select(stmt);

  const size_t mark = undo_.size();
  const bool implicit = !in_txn_;
  if (implicit) begin_txn_scope();
  try {
    ResultSet rs;
    switch (stmt.kind) {
      case Statement::Kind::CreateTable: rs = exec_create(stmt); break;
      case Statement::Kind::CreateVectorIndex: rs = exec_create_vector_index(stmt); break;
      case Statement::Kind::DropTable: rs = exec_drop_table(stmt); break;
      case Statement::Kind::Insert: rs = exec_insert(stmt); break;
      case Statement::Kind::Update: rs = exec_update(stmt); break;
      case Statement::Kind::Delete: rs = exec_delete(stmt); break;
      default: throw_sql_error(kErrInternal, "unhandled statement kind");
    }
    if (implicit) {
      commit_txn();
      undo_.clear();
    }
    return rs;
  } catch (...) {
    rollback_to(mark);
    if (implicit) release_current_txn();
    throw;
  }
}

}  // namespace mvdb
