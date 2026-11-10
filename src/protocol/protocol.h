#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "exec/engine.h"
#include "sql/sql.h"

namespace mvdb {

// Wire format. Protocol mode implements the official JSONL request/response
// envelope; text mode is kept for local SQL-level tests.
enum class OutputFormat { Protocol, Text };

// All wire-format knobs live here so that the exact output contract can be
// re-pointed without touching the engine.
struct FormatSpec {
  enum class DmlOutput { None, AffectedRows, Ok };

  OutputFormat format = OutputFormat::Protocol;
  bool print_header = true;
  bool header_when_empty = true;
  std::string column_separator = "\t";
  std::string float_format = "%.6f";
  DmlOutput dml_output = DmlOutput::None;
  std::string error_format = "ERROR {code}: {message}";
  std::string ready_marker;  // empty: nothing is printed on startup
  std::string ok_format = "OK";
};

FormatSpec load_format_spec_from_env();

// Semantic error name used by JSON output, e.g. "duplicate_key".
const char* error_name(int code);

// Splits a byte stream into statements on ';' while respecting string literals
// and comments. A trailing fragment is flushed explicitly at end of input.
class StatementSplitter {
 public:
  void feed(std::string_view chunk);
  // Extracts the next complete statement as a view into the internal buffer.
  // The view is valid until the next feed().
  bool next(std::string_view& out);
  // Returns the remaining buffered text (used at end of input).
  std::string_view flush_remaining();
  // True when no partial statement is pending.
  bool empty() const;
  // Trimmed pending text, without consuming it.
  std::string_view pending() const;
  void clear_pending();

 private:
  void compact();

  std::string buf_;
  size_t start_ = 0;  // first unconsumed byte
  size_t scan_ = 0;
};

class OutputWriter {
 public:
  explicit OutputWriter(FormatSpec spec) : spec_(std::move(spec)) {}

  void write_ready();
  void write_result(const ResultSet& rs);
  void write_error(const SqlError& err);
  void flush();

  // Protocol mode: one response object per request, echoing the raw id token.
  void write_response(const std::string& id_text, const ResultSet& rs);
  void write_error_response(const std::string& id_text, const SqlError& err);
  void write_ok_response(const std::string& id_text);

 private:
  void emit_line(const std::string& line);
  void append_rows_json(std::string& out, const ResultSet& rs) const;
  void write_result_json(const ResultSet& rs);
  void write_result_text(const ResultSet& rs);

  FormatSpec spec_;
  std::string buf_;
};

}  // namespace mvdb
