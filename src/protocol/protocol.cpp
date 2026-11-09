#include "protocol/protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "util/util.h"

namespace mvdb {

namespace {

std::string unescape_separator(std::string_view raw) {
  std::string out;
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      char next = raw[i + 1];
      if (next == 't') {
        out.push_back('\t');
        ++i;
        continue;
      }
      if (next == 'n') {
        out.push_back('\n');
        ++i;
        continue;
      }
      if (next == 'r') {
        out.push_back('\r');
        ++i;
        continue;
      }
    }
    out.push_back(raw[i]);
  }
  return out;
}

std::string env_string(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr) return fallback;
  return std::string(value);
}

bool env_bool(const char* name, bool fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr) return fallback;
  return !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0);
}

std::string replace_all(std::string text, const std::string& needle, const std::string& value) {
  size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    text.replace(pos, needle.size(), value);
    pos += value.size();
  }
  return text;
}

}  // namespace

FormatSpec load_format_spec_from_env() {
  FormatSpec spec;
  spec.print_header = env_bool("MVDB_PRINT_HEADER", spec.print_header);
  spec.header_when_empty = env_bool("MVDB_HEADER_WHEN_EMPTY", spec.header_when_empty);
  spec.column_separator = unescape_separator(env_string("MVDB_SEP", spec.column_separator));
  spec.float_format = env_string("MVDB_FLOAT_FMT", spec.float_format);
  spec.error_format = env_string("MVDB_ERROR_FMT", spec.error_format);
  spec.ready_marker = env_string("MVDB_READY", spec.ready_marker);
  spec.ok_format = env_string("MVDB_OK_FMT", spec.ok_format);
  const std::string dml = to_lower(env_string("MVDB_DML_OUTPUT", "none"));
  if (dml == "affected") {
    spec.dml_output = FormatSpec::DmlOutput::AffectedRows;
  } else if (dml == "ok") {
    spec.dml_output = FormatSpec::DmlOutput::Ok;
  } else {
    spec.dml_output = FormatSpec::DmlOutput::None;
  }
  return spec;
}

void StatementSplitter::feed(std::string_view chunk) {
  buf_.append(chunk);
  buf_.push_back('\n');
}

bool StatementSplitter::next(std::string& out) {
  bool in_string = false;
  bool in_line_comment = false;
  bool in_block_comment = false;
  for (size_t i = scan_; i < buf_.size(); ++i) {
    char c = buf_[i];
    if (in_line_comment) {
      if (c == '\n') in_line_comment = false;
      continue;
    }
    if (in_block_comment) {
      if (c == '*' && i + 1 < buf_.size() && buf_[i + 1] == '/') {
        in_block_comment = false;
        ++i;
      }
      continue;
    }
    if (in_string) {
      if (c == '\'') {
        if (i + 1 < buf_.size() && buf_[i + 1] == '\'') {
          ++i;
        } else {
          in_string = false;
        }
      }
      continue;
    }
    if (c == '\'') {
      in_string = true;
      continue;
    }
    if (c == '-' && i + 1 < buf_.size() && buf_[i + 1] == '-') {
      in_line_comment = true;
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < buf_.size() && buf_[i + 1] == '*') {
      in_block_comment = true;
      ++i;
      continue;
    }
    if (c == ';') {
      out = buf_.substr(0, i);
      buf_.erase(0, i + 1);
      scan_ = 0;
      return true;
    }
  }
  scan_ = buf_.size();
  return false;
}

std::string StatementSplitter::flush_remaining() {
  std::string rest = trim(buf_);
  buf_.clear();
  scan_ = 0;
  return rest;
}

bool StatementSplitter::empty() const { return trim(buf_).empty(); }

std::string StatementSplitter::pending() const { return trim(buf_); }

void StatementSplitter::clear_pending() {
  buf_.clear();
  scan_ = 0;
}

void OutputWriter::emit_raw(const std::string& text) { buf_ += text; }

void OutputWriter::emit_line(const std::string& line) {
  buf_ += line;
  buf_ += '\n';
}

void OutputWriter::write_ready() {
  if (spec_.ready_marker.empty()) return;
  emit_line(spec_.ready_marker);
  flush();
}

void OutputWriter::write_result(const ResultSet& rs) {
  if (rs.has_rows) {
    if (rs.rows.empty() && !spec_.header_when_empty) return;
    if (spec_.print_header) {
      std::string line;
      for (size_t i = 0; i < rs.columns.size(); ++i) {
        if (i != 0) line += spec_.column_separator;
        line += rs.columns[i];
      }
      emit_line(line);
    }
    for (const std::vector<std::string>& row : rs.rows) {
      std::string line;
      for (size_t i = 0; i < row.size(); ++i) {
        if (i != 0) line += spec_.column_separator;
        line += row[i];
      }
      emit_line(line);
    }
    return;
  }
  switch (spec_.dml_output) {
    case FormatSpec::DmlOutput::None:
      break;
    case FormatSpec::DmlOutput::AffectedRows:
      emit_line("affected_rows: " + std::to_string(rs.affected_rows));
      break;
    case FormatSpec::DmlOutput::Ok:
      emit_line(spec_.ok_format);
      break;
  }
}

void OutputWriter::write_error(const SqlError& err) {
  std::string text = replace_all(spec_.error_format, "{code}", std::to_string(err.code));
  text = replace_all(text, "{message}", err.message);
  emit_line(text);
}

void OutputWriter::flush() {
  if (buf_.empty()) return;
  std::fwrite(buf_.data(), 1, buf_.size(), stdout);
  std::fflush(stdout);
  buf_.clear();
}

}  // namespace mvdb
