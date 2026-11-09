#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "exec/engine.h"
#include "protocol/protocol.h"
#include "sql/sql.h"
#include "util/util.h"

namespace {

struct Options {
  std::string data_dir = "/data";
  bool probe = false;
  bool stats = false;
};

// Reports this process's own peak RSS and block I/O on stderr. Used by local
// profiling only; the platform measures both from cgroup counters.
void report_process_stats() {
  {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
      if (line.rfind("VmHWM:", 0) == 0) {
        std::fprintf(stderr, "[stats] peak_rss_kb=%s\n", line.c_str() + 6);
        break;
      }
    }
  }
  std::ifstream io("/proc/self/io");
  std::string line;
  unsigned long long read_bytes = 0;
  unsigned long long write_bytes = 0;
  while (std::getline(io, line)) {
    if (line.rfind("read_bytes:", 0) == 0) read_bytes = std::strtoull(line.c_str() + 11, nullptr, 10);
    if (line.rfind("write_bytes:", 0) == 0) write_bytes = std::strtoull(line.c_str() + 12, nullptr, 10);
  }
  std::fprintf(stderr, "[stats] read_bytes=%llu write_bytes=%llu\n", read_bytes, write_bytes);
}

void print_help() {
  std::fprintf(stderr,
               "minivectordb - lightweight vector database\n"
               "Usage: minivectordb [--data-dir DIR] [--probe] [--stats] [--help] [--version]\n"
               "\n"
               "Reads SQL statements from stdin, writes results to stdout and\n"
               "diagnostics to stderr. One statement is terminated by ';'.\n"
               "\n"
               "Environment (wire format):\n"
               "  MVDB_PRINT_HEADER  1|0           print a header row for result sets\n"
               "  MVDB_SEP           separator     column separator, default \\t\n"
               "  MVDB_FLOAT_FMT     printf fmt    distance format, default %%.6f\n"
               "  MVDB_DML_OUTPUT    none|affected|ok\n"
               "  MVDB_ERROR_FMT     template      placeholders {code} {message}\n"
               "  MVDB_READY         text          readiness marker printed on startup\n"
               "  MVDB_OK_FMT        text          text used by MVDB_DML_OUTPUT=ok\n");
}

bool is_exit_command(const std::string& text) {
  const std::string t = mvdb::to_lower(mvdb::trim(text));
  return t == "quit" || t == "exit" || t == "\\q" || t == "bye";
}

// Returns false when the session should end.
// Detects a leading `/* mode=ann */` style hint. The official task package
// decides how exact/ann is selected; this keeps a second channel available in
// case the driver uses a comment instead of a statement.
bool apply_mode_comment(const std::string& text, mvdb::Engine& engine) {
  const size_t open = text.find("/*");
  if (open == std::string::npos) return false;
  const size_t close = text.find("*/", open + 2);
  if (close == std::string::npos) return false;
  const std::string body = mvdb::to_lower(text.substr(open + 2, close - open - 2));
  const size_t pos = body.find("mode");
  if (pos == std::string::npos) return false;
  const size_t eq = body.find('=', pos);
  if (eq == std::string::npos) return false;
  const std::string value = mvdb::trim(body.substr(eq + 1));
  if (value == "ann") {
    engine.set_mode(mvdb::Engine::QueryMode::Ann);
    return true;
  }
  if (value == "exact") {
    engine.set_mode(mvdb::Engine::QueryMode::Exact);
    return true;
  }
  return false;
}

bool handle_statement(const std::string& raw, mvdb::Engine& engine, mvdb::OutputWriter& writer,
                      const Options& opts) {
  const std::string text = mvdb::trim(raw);
  if (text.empty()) return true;
  if (is_exit_command(text)) return false;
  apply_mode_comment(text, engine);

  if (opts.probe) {
    std::fprintf(stderr, "[probe] statement: %s\n", text.c_str());
  }

  try {
    mvdb::Statement stmt = mvdb::parse_statement(text);
    mvdb::ResultSet rs = engine.execute(stmt);
    if (opts.probe) {
      std::fprintf(stderr, "[probe] result: rows=%zu affected=%lld\n", rs.rows.size(),
                   static_cast<long long>(rs.affected_rows));
    }
    writer.write_result(rs);
  } catch (const mvdb::SqlError& err) {
    if (opts.probe) {
      std::fprintf(stderr, "[probe] error: %d %s\n", err.code, err.message.c_str());
    }
    writer.write_error(err);
  } catch (const std::exception& ex) {
    mvdb::SqlError err{mvdb::kErrInternal, ex.what()};
    writer.write_error(err);
  }
  writer.flush();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--data-dir" && i + 1 < argc) {
      opts.data_dir = argv[++i];
    } else if (arg.rfind("--data-dir=", 0) == 0) {
      opts.data_dir = arg.substr(std::strlen("--data-dir="));
    } else if (arg == "--probe") {
      opts.probe = true;
    } else if (arg == "--stats") {
      opts.stats = true;
    } else if (arg == "--help" || arg == "-h") {
      print_help();
      return 0;
    } else if (arg == "--version") {
      std::printf("minivectordb 0.1.0\n");
      return 0;
    } else {
      std::fprintf(stderr, "error: unknown argument '%s' (see --help)\n", arg.c_str());
      return 2;
    }
  }

  std::signal(SIGPIPE, SIG_IGN);

  mvdb::FormatSpec spec = mvdb::load_format_spec_from_env();
  size_t pool_pages = 2048;
  if (const char* value = std::getenv("MVDB_POOL_PAGES")) {
    pool_pages = std::strtoul(value, nullptr, 10);
  }
  mvdb::Engine engine(opts.data_dir, pool_pages);
  {
    std::string error;
    if (!engine.open(error)) {
      std::fprintf(stderr, "fatal: %s\n", error.c_str());
      return 1;
    }
  }
  engine.set_float_format(spec.float_format);
  mvdb::OutputWriter writer(spec);

  if (opts.probe) {
    std::fprintf(stderr, "[probe] data-dir=%s\n", opts.data_dir.c_str());
  }
  writer.write_ready();

  mvdb::StatementSplitter splitter;
  std::string line;
  std::string statement;
  bool running = true;
  while (running && std::getline(std::cin, line)) {
    // A bare exit command on its own line ends the session even though it is
    // not terminated by ';'.
    if (splitter.empty() && is_exit_command(line)) break;
    splitter.feed(line);
    while (splitter.next(statement)) {
      if (!handle_statement(statement, engine, writer, opts)) {
        running = false;
        break;
      }
    }
    // Fallback for a driver that terminates statements with a newline instead
    // of ';': if the buffered text already parses, run it now. The parser
    // reports "incomplete" for genuinely unfinished statements, which keeps
    // multi-line statements working in both modes.
    // Only single-line statements are eligible: a buffer spanning several
    // lines means the driver is using ';' termination and a multi-line
    // statement, which must not be executed early.
    if (running && !splitter.empty()) {
      const std::string pending = splitter.pending();
      bool complete = pending.find('\n') == std::string::npos;
      try {
        mvdb::parse_statement(pending);
      } catch (const mvdb::SqlError& err) {
        if (err.code == mvdb::kIncomplete) complete = false;
      } catch (...) {
      }
      if (complete) {
        splitter.clear_pending();
        if (!handle_statement(pending, engine, writer, opts)) running = false;
      }
    }
  }
  if (running) {
    const std::string rest = splitter.flush_remaining();
    if (!rest.empty()) {
      handle_statement(rest, engine, writer, opts);
    }
  }
  writer.flush();
  engine.shutdown();
  if (opts.stats) {
    report_process_stats();
    const mvdb::Engine::Timings& t = engine.timings();
    std::fprintf(stderr,
                 "[stats] queries=%llu ann_queries=%llu rows_scanned=%llu avg_ef=%.1f\n"
                 "[stats] select_scan_ms=%.1f select_ann_ms=%.1f commit_ms=%.1f index_build_ms=%.1f\n",
                 static_cast<unsigned long long>(t.queries),
                 static_cast<unsigned long long>(t.ann_queries),
                 static_cast<unsigned long long>(t.rows_scanned),
                 t.ann_queries ? static_cast<double>(t.ann_ef_total) / static_cast<double>(t.ann_queries) : 0.0,
                 t.select_scan_ms, t.select_ann_ms, t.commit_ms, t.index_build_ms);
  }
  return 0;
}
