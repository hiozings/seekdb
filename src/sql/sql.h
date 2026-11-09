#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mvdb {

// Error codes follow the MySQL convention so that the protocol layer can remap
// them once the official task package publishes its own table.
enum ErrCode : int {
  kErrSyntax = 1064,
  kErrUnknownColumn = 1054,
  kErrTableExists = 1050,
  kErrTableNotFound = 1146,
  kErrDuplicateKey = 1062,
  kErrIncorrectValue = 1366,
  kErrOutOfRange = 1264,
  kErrNotSupported = 1235,
  kErrNoPrimaryKey = 1173,
  kErrMultiplePrimaryKey = 1068,
  kErrDuplicateColumn = 1060,
  kErrInternal = 1105,
};

struct SqlError {
  int code;
  std::string message;
};

[[noreturn]] void throw_sql_error(int code, const std::string& message);

enum class ColumnType { BigInt, Varchar, Vector };

struct Value {
  // Float is an intermediate kind produced only by the parser; the engine
  // rejects it wherever an integer is required.
  enum class Kind { Null, Int, Float, Str, Vec };
  Kind kind = Kind::Null;
  int64_t i = 0;
  double d = 0.0;
  std::string s;
  std::vector<float> v;

  static Value make_int(int64_t value);
  static Value make_str(std::string value);
  static Value make_vec(std::vector<float> value);
};

struct ColumnDef {
  std::string name;
  ColumnType type = ColumnType::BigInt;
  int64_t length = 0;   // VARCHAR(n) character count
  int64_t dim = 0;      // VECTOR(n) dimension
  bool primary_key = false;
};

struct Condition {
  enum class Op { Eq, Lt, Le, Gt, Ge };
  std::string column;
  Op op = Op::Eq;
  Value literal;
};

struct SelectItem {
  bool is_distance = false;
  std::string column;       // source column, or argument of L2_DISTANCE
  std::string alias;        // optional AS alias
  std::vector<float> query; // L2_DISTANCE second argument
};

struct OrderKey {
  std::string name;
  bool asc = true;
};

struct Statement {
  enum class Kind {
    CreateTable,
    Insert,
    Select,
    Update,
    Delete,
    Begin,
    Commit,
    Rollback,
  };

  Kind kind = Kind::Select;

  // CREATE TABLE
  std::string table;
  std::vector<ColumnDef> columns;

  // INSERT
  std::vector<std::string> insert_columns;  // empty means all columns in order
  std::vector<std::vector<Value>> rows;

  // SELECT
  bool select_star = false;
  std::vector<SelectItem> select_items;
  bool has_where = false;
  std::vector<Condition> where;
  std::vector<OrderKey> order_by;
  bool has_limit = false;
  int64_t limit = 0;

  // UPDATE
  std::vector<std::pair<std::string, Value>> set_items;
};

// Parses a textual vector literal such as "[1, 2, 3]".
// Throws SqlError when the text is malformed or a value is not finite float32.
std::vector<float> parse_vector_literal(const std::string& text);

// Parses one statement without the trailing semicolon.
// Throws SqlError on syntax errors or unsupported constructs.
Statement parse_statement(const std::string& sql);

}  // namespace mvdb
