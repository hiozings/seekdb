#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "exec/engine.h"
#include "sql/sql.h"

namespace mvdb {

// All wire-format knobs live here so that the exact stdout contract can be
// re-pointed at the official task package without touching the engine.
struct FormatSpec {
  enum class DmlOutput { None, AffectedRows, Ok };

  bool print_header = true;
  std::string column_separator = "\t";
  std::string float_format = "%.6f";
  DmlOutput dml_output = DmlOutput::None;
  std::string error_format = "ERROR {code}: {message}";
  std::string ready_marker;  // empty: nothing is printed on startup
  std::string ok_format = "OK";
};

FormatSpec load_format_spec_from_env();

// Splits a byte stream into statements on ';' while respecting string literals
// and comments. A trailing fragment is flushed explicitly at end of input.
class StatementSplitter {
 public:
  void feed(std::string_view chunk);
  // Extracts the next complete statement, if any.
  bool next(std::string& out);
  // Returns the remaining buffered text (used at end of input).
  std::string flush_remaining();
  // True when no partial statement is pending.
  bool empty() const;
  // Trimmed pending text, without consuming it.
  std::string pending() const;
  void clear_pending();

 private:
  std::string buf_;
  size_t scan_ = 0;
};

class OutputWriter {
 public:
  explicit OutputWriter(FormatSpec spec) : spec_(std::move(spec)) {}

  void write_ready();
  void write_result(const ResultSet& rs);
  void write_error(const SqlError& err);
  void flush();

 private:
  void emit_line(const std::string& line);
  void emit_raw(const std::string& text);

  FormatSpec spec_;
  std::string buf_;
};

}  // namespace mvdb
