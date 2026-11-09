#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sql/sql.h"
#include "storage/page.h"
#include "storage/schema.h"
#include "txn/wal.h"

namespace mvdb {

class BTree;
class HeapFile;
class VectorIndex;

// A WHERE clause resolved against a schema: column indexes and coerced values
// are computed once per query instead of per row.
struct PreparedCondition;

struct ResultSet {
  bool has_rows = false;
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  int64_t affected_rows = 0;
};

// Durable execution engine. Rows live in heap files under the data directory;
// the catalog file is the authoritative schema store.
class Engine {
 public:
  explicit Engine(std::string data_dir, size_t pool_pages = 2048);
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Loads the catalog, opens heap files and rebuilds the primary-key index.
  bool open(std::string& error);

  void set_float_format(std::string fmt) { float_fmt_ = std::move(fmt); }

  // Phase counters used by --stats to make optimization measurable.
  struct Timings {
    double select_scan_ms = 0.0;
    double select_ann_ms = 0.0;
    double commit_ms = 0.0;
    double index_build_ms = 0.0;
    uint64_t rows_scanned = 0;
    uint64_t queries = 0;
    uint64_t ann_queries = 0;
    uint64_t ann_ef_total = 0;
    uint64_t wal_bytes = 0;
  };
  const Timings& timings() const { return timings_; }

  enum class QueryMode { Exact, Ann };
  void set_mode(QueryMode mode) { mode_ = mode; }
  QueryMode mode() const { return mode_; }
  void set_ef_floor(size_t ef) { ef_floor_ = ef; }

  // Executes one statement. Throws SqlError on failure; a failed statement
  // leaves no partial modification behind.
  ResultSet execute(const Statement& stmt);

  // Rolls back an open transaction and flushes everything to disk. Called on
  // normal shutdown so that an interrupted session cannot leak uncommitted data.
  void shutdown();

 private:
  struct TableState {
    TableMeta meta;
    std::unique_ptr<HeapFile> heap;
    std::unique_ptr<BTree> pk_index;
    std::unique_ptr<VectorIndex> vec_index;
    // Set when a rollback may have left the graph stale; rebuilt before use.
    bool vec_dirty = false;
  };

  struct UndoOp {
    enum class Kind { InsertedRow, DeletedRow, UpdatedRow, CreatedTable, CreatedVectorIndex } kind;
    uint32_t table_id = 0;
    int64_t pk = 0;
    RID rid = 0;
    std::string record;
  };

  TableState& require_table(const std::string& name);
  const TableState& require_table(const std::string& name) const;
  size_t require_column(const TableMeta& meta, const std::string& name) const;
  Value coerce(const TableMeta& meta, size_t column_index, const Value& input) const;
  bool matches(const TableMeta& meta, const std::vector<Value>& row,
               const std::vector<Condition>& conds) const;
  std::vector<PreparedCondition> prepare_filter(const TableMeta& meta,
                                                const std::vector<Condition>& conds) const;
  bool matches_prepared(const std::vector<PreparedCondition>& prepared,
                        const std::vector<Value>& row) const;

  // Allocation-free access to the columns a Top-K query needs, resolved
  // directly on the encoded row bytes.
  struct RawRow {
    const char* pk = nullptr;
    const float* vec = nullptr;
    std::vector<const char*> filter_columns;
  };
  bool locate_raw_row(const TableMeta& meta, std::string_view bytes,
                      const std::vector<PreparedCondition>& prepared, int vector_column,
                      RawRow& out) const;
  bool raw_row_matches(const std::vector<PreparedCondition>& prepared, const RawRow& raw) const;

  ResultSet exec_create(const Statement& stmt);
  ResultSet exec_insert(const Statement& stmt);
  ResultSet exec_select(const Statement& stmt);
  ResultSet exec_update(const Statement& stmt);
  ResultSet exec_delete(const Statement& stmt);
  ResultSet exec_create_vector_index(const Statement& stmt);
  ResultSet exec_set_mode(const Statement& stmt);
  void build_vector_index(TableState& table);

  void rollback_to(size_t mark);
  void begin_txn_scope();
  void release_current_txn();
  void commit_txn();
  bool recover(const std::vector<TableMeta>& metas, std::string& error);
  void drop_table(uint32_t table_id);
  bool flush_all();
  bool checkpoint();
  bool save_catalog();
  std::string heap_path(uint32_t table_id) const;
  std::string btree_path(uint32_t table_id) const;

  std::string format_value(const Value& value, ColumnType type) const;
  double l2_distance(const std::vector<float>& a, const std::vector<float>& b) const;

  std::string data_dir_;
  std::string catalog_path_;
  std::string wal_path_;
  size_t pool_pages_;
  uint32_t next_table_id_ = 1;
  std::map<uint32_t, std::unique_ptr<TableState>> tables_;
  std::map<std::string, uint32_t> by_name_;
  std::vector<UndoOp> undo_;
  Wal wal_;
  uint64_t next_txid_ = 1;
  uint64_t current_txn_ = 0;
  size_t wal_bytes_since_checkpoint_ = 0;
  size_t checkpoint_threshold_ = 64 * 1024 * 1024;
  bool in_txn_ = false;
  std::string float_fmt_ = "%.6f";
  Timings timings_;
  QueryMode mode_ = QueryMode::Exact;
  size_t ef_floor_ = 256;
  size_t ef_max_ = 1024;
};

}  // namespace mvdb
