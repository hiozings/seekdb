#include "sql/sql.h"

#include <charconv>
#include <cmath>
#include <system_error>
#include <cctype>
#include <deque>
#include <limits>
#include <utility>

#include "util/util.h"

namespace mvdb {

[[noreturn]] void throw_sql_error(int code, const std::string& message) {
  throw SqlError{code, message};
}

Value Value::make_int(int64_t value) {
  Value v;
  v.kind = Kind::Int;
  v.i = value;
  return v;
}

Value Value::make_str(std::string value) {
  Value v;
  v.kind = Kind::Str;
  v.s = std::move(value);
  return v;
}

Value Value::make_vec(std::vector<float> value) {
  Value v;
  v.kind = Kind::Vec;
  v.v = std::move(value);
  return v;
}

namespace {

enum class TokenKind {
  End,
  Ident,
  IntLit,
  FloatLit,
  StrLit,
  LParen,
  RParen,
  Comma,
  Star,
  Eq,
  Lt,
  Le,
  Gt,
  Ge,
  LBracket,
  RBracket,
  Minus,
  Plus,
};

// Tokens point into the statement buffer. Only string literals that need
// unescaping are materialised, in `owned` (a deque, so views stay valid).
struct Token {
  TokenKind kind = TokenKind::End;
  std::string_view text;
  size_t pos = 0;
};

struct TokenStream {
  std::vector<Token> tokens;
  std::deque<std::string> owned;
};

[[noreturn]] void syntax_error(const std::string& detail) {
  throw_sql_error(kErrSyntax, "syntax error: " + detail);
}

[[noreturn]] void incomplete(const std::string& detail) {
  throw SqlError{kIncomplete, detail};
}

TokenStream tokenize(std::string_view sql) {
  TokenStream stream;
  std::vector<Token>& out = stream.tokens;
  size_t i = 0;
  const size_t n = sql.size();
  auto is_ident_start = [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
  };
  auto is_ident_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  while (i < n) {
    char c = sql[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      size_t end = sql.find("*/", i + 2);
      if (end == std::string::npos) syntax_error("unterminated comment");
      i = end + 2;
      continue;
    }
    if (is_ident_start(c)) {
      size_t start = i;
      while (i < n && is_ident_char(sql[i])) ++i;
      out.push_back(Token{TokenKind::Ident, sql.substr(start, i - start), start});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      size_t start = i;
      bool is_float = false;
      while (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) ++i;
      if (i < n && sql[i] == '.') {
        is_float = true;
        ++i;
        while (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) ++i;
      }
      if (i < n && (sql[i] == 'e' || sql[i] == 'E')) {
        size_t save = i;
        ++i;
        if (i < n && (sql[i] == '+' || sql[i] == '-')) ++i;
        if (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) {
          is_float = true;
          while (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) ++i;
        } else {
          i = save;
        }
      }
      out.push_back(Token{is_float ? TokenKind::FloatLit : TokenKind::IntLit,
                          sql.substr(start, i - start), start});
      continue;
    }
    if (c == '\'') {
      const size_t start = i;
      ++i;
      bool escaped = false;
      bool closed = false;
      while (i < n) {
        if (sql[i] == '\'') {
          if (i + 1 < n && sql[i + 1] == '\'') {
            escaped = true;
            i += 2;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        ++i;
      }
      if (!closed) syntax_error("unterminated string literal");
      if (!escaped) {
        // Common case: a plain literal, referenced in place with no copy.
        out.push_back(Token{TokenKind::StrLit, sql.substr(start + 1, i - 1 - (start + 1)), start});
      } else {
        std::string value;
        value.reserve(i - start);
        size_t k = start + 1;
        while (k < i - 1) {
          if (sql[k] == '\'' && k + 1 < i - 1 && sql[k + 1] == '\'') {
            value.push_back('\'');
            k += 2;
            continue;
          }
          value.push_back(sql[k]);
          ++k;
        }
        stream.owned.push_back(std::move(value));
        out.push_back(Token{TokenKind::StrLit, stream.owned.back(), start});
      }
      continue;
    }
    switch (c) {
      case '(': out.push_back(Token{TokenKind::LParen, "(", i}); ++i; break;
      case ')': out.push_back(Token{TokenKind::RParen, ")", i}); ++i; break;
      case ',': out.push_back(Token{TokenKind::Comma, ",", i}); ++i; break;
      case '*': out.push_back(Token{TokenKind::Star, "*", i}); ++i; break;
      case '[': out.push_back(Token{TokenKind::LBracket, "[", i}); ++i; break;
      case ']': out.push_back(Token{TokenKind::RBracket, "]", i}); ++i; break;
      case '+': out.push_back(Token{TokenKind::Plus, "+", i}); ++i; break;
      case '-': out.push_back(Token{TokenKind::Minus, "-", i}); ++i; break;
      case '=': out.push_back(Token{TokenKind::Eq, "=", i}); ++i; break;
      case '<':
        if (i + 1 < n && sql[i + 1] == '=') {
          out.push_back(Token{TokenKind::Le, "<=", i});
          i += 2;
        } else {
          out.push_back(Token{TokenKind::Lt, "<", i});
          ++i;
        }
        break;
      case '>':
        if (i + 1 < n && sql[i + 1] == '=') {
          out.push_back(Token{TokenKind::Ge, ">=", i});
          i += 2;
        } else {
          out.push_back(Token{TokenKind::Gt, ">", i});
          ++i;
        }
        break;
      default:
        syntax_error(std::string("unexpected character '") + c + "'");
    }
  }
  out.push_back(Token{TokenKind::End, std::string_view(), n});
  return stream;
}

// Converts a double to float, rejecting overflow and non-finite values.
float to_float32(double value, const char* what) {
  if (!std::isfinite(value)) {
    throw_sql_error(kErrVectorValue, std::string("non-finite value in ") + what);
  }
  float f = static_cast<float>(value);
  if (!std::isfinite(f)) {
    throw_sql_error(kErrVectorOverflow, std::string("value out of float32 range in ") + what);
  }
  return f;
}

std::vector<float> parse_vector_text(std::string_view text) {
  // Parsed in place: the previous implementation split the text into one
  // std::string per element and trimmed each, which dominated bulk inserts.
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  if (end - begin < 2 || text[begin] != '[' || text[end - 1] != ']') {
    throw_sql_error(kErrIncorrectValue, "vector literal must be [v1, v2, ...]");
  }
  std::string_view inner = text.substr(begin + 1, end - begin - 2);
  std::vector<float> out;
  out.reserve(inner.size() / 8 + 1);
  size_t i = 0;
  while (i < inner.size()) {
    while (i < inner.size() &&
           (inner[i] == ' ' || inner[i] == '\t' || inner[i] == ',' || inner[i] == '\n')) {
      ++i;
    }
    if (i >= inner.size()) break;
    const char* first = inner.data() + i;
    const char* last = inner.data() + inner.size();
    double value = 0.0;
    const auto result = std::from_chars(first, last, value, std::chars_format::general);
    if (result.ec != std::errc() || result.ptr == first) {
      throw_sql_error(kErrIncorrectValue,
                      "invalid number in vector literal: " + std::string(first, 16));
    }
    out.push_back(to_float32(value, "vector literal"));
    i = static_cast<size_t>(result.ptr - inner.data());
    if (i < inner.size() && inner[i] != ',' && inner[i] != ' ' && inner[i] != '\t' &&
        inner[i] != '\n') {
      throw_sql_error(kErrIncorrectValue, "invalid character in vector literal");
    }
  }
  return out;
}

class Parser {
 public:
  explicit Parser(TokenStream stream)
      : toks_(std::move(stream.tokens)), owned_(std::move(stream.owned)) {}

  Statement parse() {
    Statement stmt;
    if (peek().kind == TokenKind::End) incomplete("empty statement");
    if (peek().kind != TokenKind::Ident) syntax_error("expected a statement keyword");
    const std::string kw = to_lower(std::string(peek().text));
    if (kw == "create") {
      if (is_kw(peek(1), "vector")) {
        parse_create_vector_index(stmt);
      } else {
        parse_create_table(stmt);
      }
    } else if (kw == "set") {
      parse_set_mode(stmt);
    } else if (kw == "use") {
      // Session database selection: accepted as a no-op, the engine has a
      // single data directory.
      advance();
      if (peek().kind == TokenKind::End) incomplete("expected a database name");
      advance();
      stmt.kind = Statement::Kind::NoOp;
    } else if (kw == "drop") {
      parse_drop_table(stmt);
    } else if (kw == "insert") {
      parse_insert(stmt);
    } else if (kw == "select") {
      parse_select(stmt);
    } else if (kw == "update") {
      parse_update(stmt);
    } else if (kw == "delete") {
      parse_delete(stmt);
    } else if (kw == "begin") {
      stmt.kind = Statement::Kind::Begin;
      advance();
      if (is_kw(peek(), "transaction") || is_kw(peek(), "work")) advance();
    } else if (kw == "start") {
      advance();
      expect_kw("transaction");
      stmt.kind = Statement::Kind::Begin;
    } else if (kw == "commit") {
      stmt.kind = Statement::Kind::Commit;
      advance();
      if (is_kw(peek(), "work")) advance();
    } else if (kw == "rollback") {
      stmt.kind = Statement::Kind::Rollback;
      advance();
      if (is_kw(peek(), "work")) advance();
    } else {
      throw_sql_error(kErrNotSupported, "unsupported statement '" + std::string(peek().text) + "'");
    }
    if (peek().kind != TokenKind::End) {
      syntax_error("unexpected trailing token '" + std::string(peek().text) + "'");
    }
    return stmt;
  }

 private:
  const Token& peek(size_t offset = 0) const {
    size_t idx = pos_ + offset;
    if (idx >= toks_.size()) return toks_.back();
    return toks_[idx];
  }

  void advance() {
    if (pos_ + 1 < toks_.size()) ++pos_;
  }

  static bool is_kw(const Token& tok, const char* kw) {
    return tok.kind == TokenKind::Ident && iequals(tok.text, std::string_view(kw));
  }

  bool accept_kw(const char* kw) {
    if (is_kw(peek(), kw)) {
      advance();
      return true;
    }
    return false;
  }

  void expect_kw(const char* kw) {
    if (accept_kw(kw)) return;
    if (peek().kind == TokenKind::End) incomplete(std::string("expected '") + kw + "'");
    syntax_error(std::string("expected '") + kw + "'");
  }

  bool accept(TokenKind kind) {
    if (peek().kind == kind) {
      advance();
      return true;
    }
    return false;
  }

  void expect(TokenKind kind, const char* what) {
    if (accept(kind)) return;
    if (peek().kind == TokenKind::End) incomplete(std::string("expected ") + what);
    syntax_error(std::string("expected ") + what);
  }

  std::string expect_ident(const char* what) {
    if (peek().kind == TokenKind::End) incomplete(std::string("expected ") + what);
    if (peek().kind != TokenKind::Ident) syntax_error(std::string("expected ") + what);
    std::string name(peek().text);
    advance();
    return name;
  }

  int64_t expect_uint(const char* what) {
    if (peek().kind == TokenKind::End) incomplete(std::string("expected ") + what);
    if (peek().kind != TokenKind::IntLit) syntax_error(std::string("expected ") + what);
    int64_t value = 0;
    if (!parse_int64(peek().text, value) || value < 0) {
      throw_sql_error(kErrOutOfRange, std::string("invalid ") + what);
    }
    advance();
    return value;
  }

  Value parse_vector_from_brackets() {
    expect(TokenKind::LBracket, "'['");
    std::vector<float> v;
    if (peek().kind != TokenKind::RBracket) {
      while (true) {
        Value element = parse_numeric_literal();
        if (element.kind == Value::Kind::Int) {
          v.push_back(to_float32(static_cast<double>(element.i), "vector literal"));
        } else if (element.kind == Value::Kind::Float) {
          v.push_back(to_float32(element.d, "vector literal"));
        } else {
          throw_sql_error(kErrIncorrectValue, "invalid element in vector literal");
        }
        if (accept(TokenKind::Comma)) continue;
        break;
      }
    }
    expect(TokenKind::RBracket, "']'");
    return Value::make_vec(std::move(v));
  }

  Value parse_numeric_literal() {
    bool negative = false;
    if (accept(TokenKind::Minus)) {
      negative = true;
    } else {
      accept(TokenKind::Plus);
    }
    if (peek().kind == TokenKind::IntLit) {
      int64_t value = 0;
      if (!parse_int64(peek().text, value)) {
        throw_sql_error(kErrOutOfRange, "invalid integer literal '" + std::string(peek().text) + "'");
      }
      advance();
      Value v;
      v.kind = Value::Kind::Int;
      v.i = negative ? -value : value;
      return v;
    }
    if (peek().kind == TokenKind::FloatLit) {
      double d = 0.0;
      if (!parse_double(peek().text, d)) {
        throw_sql_error(kErrOutOfRange, "invalid float literal '" + std::string(peek().text) + "'");
      }
      advance();
      Value v;
      v.kind = Value::Kind::Float;
      v.d = negative ? -d : d;
      return v;
    }
    syntax_error("expected a numeric literal");
  }

  Value parse_literal() {
    if (peek().kind == TokenKind::StrLit) {
      const std::string_view text = peek().text;
      advance();
      std::string_view trimmed = text;
      while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.remove_prefix(1);
      }
      while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.remove_suffix(1);
      }
      if (!trimmed.empty() && trimmed.front() == '[') {
        return Value::make_vec(parse_vector_text(text));
      }
      return Value::make_str(std::string(text));
    }
    if (peek().kind == TokenKind::LBracket) {
      return parse_vector_from_brackets();
    }
    if (peek().kind == TokenKind::Minus || peek().kind == TokenKind::Plus ||
        peek().kind == TokenKind::IntLit || peek().kind == TokenKind::FloatLit) {
      return parse_numeric_literal();
    }
    if (peek().kind == TokenKind::End) incomplete("expected a literal");
    syntax_error("expected a literal");
  }

  void parse_drop_table(Statement& stmt) {
    stmt.kind = Statement::Kind::DropTable;
    expect_kw("drop");
    expect_kw("table");
    if (accept_kw("if")) {
      expect_kw("exists");
      stmt.if_exists = true;
    }
    stmt.table = expect_ident("a table name");
  }

  void parse_create_vector_index(Statement& stmt) {
    stmt.kind = Statement::Kind::CreateVectorIndex;
    expect_kw("create");
    expect_kw("vector");
    expect_kw("index");
    stmt.index_name = expect_ident("an index name");
    expect_kw("on");
    stmt.table = expect_ident("a table name");
    expect(TokenKind::LParen, "'('");
    stmt.index_column = expect_ident("a column name");
    expect(TokenKind::RParen, "')'");
  }

  void parse_set_mode(Statement& stmt) {
    expect_kw("set");
    if (!is_kw(peek(), "mode")) {
      // Drivers commonly issue session settings (SET NAMES, SET autocommit,
      // SET SESSION ...). They do not change engine behaviour, so accept them
      // instead of failing the whole test case.
      while (peek().kind != TokenKind::End) advance();
      stmt.kind = Statement::Kind::NoOp;
      return;
    }
    stmt.kind = Statement::Kind::SetMode;
    advance();
    accept(TokenKind::Eq);
    if (peek().kind == TokenKind::StrLit) {
      stmt.mode_value = peek().text;
      advance();
    } else if (peek().kind == TokenKind::Ident) {
      stmt.mode_value = peek().text;
      advance();
    } else {
      syntax_error("expected 'exact' or 'ann'");
    }
  }

  void parse_create_table(Statement& stmt) {
    stmt.kind = Statement::Kind::CreateTable;
    expect_kw("create");
    expect_kw("table");
    if (accept_kw("if")) {
      expect_kw("not");
      expect_kw("exists");
      stmt.if_not_exists = true;
    }
    stmt.table = expect_ident("a table name");
    expect(TokenKind::LParen, "'('");
    size_t primary_keys = 0;
    size_t vector_columns = 0;
    while (true) {
      ColumnDef col;
      col.name = expect_ident("a column name");
      if (peek().kind != TokenKind::Ident) syntax_error("expected a column type");
      const std::string type = to_lower(peek().text);
      advance();
      if (type == "bigint") {
        col.type = ColumnType::BigInt;
      } else if (type == "varchar") {
        col.type = ColumnType::Varchar;
        expect(TokenKind::LParen, "'('");
        col.length = expect_uint("a VARCHAR length");
        expect(TokenKind::RParen, "')'");
        if (col.length <= 0) throw_sql_error(kErrOutOfRange, "VARCHAR length must be positive");
      } else if (type == "vector") {
        col.type = ColumnType::Vector;
        expect(TokenKind::LParen, "'('");
        col.dim = expect_uint("a VECTOR dimension");
        expect(TokenKind::RParen, "')'");
        if (col.dim <= 0) throw_sql_error(kErrOutOfRange, "VECTOR dimension must be positive");
        ++vector_columns;
      } else {
        throw_sql_error(kErrNotSupported, "unsupported column type '" + type + "'");
      }
      while (is_kw(peek(), "primary") || is_kw(peek(), "not") || is_kw(peek(), "null") ||
             is_kw(peek(), "unique")) {
        if (accept_kw("primary")) {
          expect_kw("key");
          col.primary_key = true;
          ++primary_keys;
        } else if (accept_kw("not")) {
          expect_kw("null");
          throw_sql_error(kErrNotSupported, "NOT NULL is not supported");
        } else if (accept_kw("null")) {
          throw_sql_error(kErrNotSupported, "NULL columns are not supported");
        } else {
          accept_kw("unique");
          throw_sql_error(kErrNotSupported, "UNIQUE is not supported");
        }
      }
      stmt.columns.push_back(std::move(col));
      if (accept(TokenKind::Comma)) continue;
      break;
    }
    expect(TokenKind::RParen, "')'");
    if (primary_keys == 0) {
      throw_sql_error(kErrNoPrimaryKey, "table must have exactly one BIGINT primary key");
    }
    if (primary_keys > 1) {
      throw_sql_error(kErrMultiplePrimaryKey, "multiple primary keys defined");
    }
    if (vector_columns > 1) {
      throw_sql_error(kErrNotSupported, "at most one VECTOR column is supported");
    }
    for (const ColumnDef& col : stmt.columns) {
      if (col.primary_key && col.type != ColumnType::BigInt) {
        throw_sql_error(kErrIncorrectValue, "primary key must be BIGINT");
      }
    }
  }

  void parse_insert(Statement& stmt) {
    stmt.kind = Statement::Kind::Insert;
    expect_kw("insert");
    expect_kw("into");
    stmt.table = expect_ident("a table name");
    if (accept(TokenKind::LParen)) {
      if (peek().kind != TokenKind::RParen) {
        while (true) {
          stmt.insert_columns.push_back(expect_ident("a column name"));
          if (accept(TokenKind::Comma)) continue;
          break;
        }
      }
      expect(TokenKind::RParen, "')'");
    }
    expect_kw("values");
    while (true) {
      expect(TokenKind::LParen, "'('");
      std::vector<Value> row;
      if (peek().kind != TokenKind::RParen) {
        while (true) {
          row.push_back(parse_literal());
          if (accept(TokenKind::Comma)) continue;
          break;
        }
      }
      expect(TokenKind::RParen, "')'");
      stmt.rows.push_back(std::move(row));
      if (accept(TokenKind::Comma)) continue;
      break;
    }
  }

  void parse_select(Statement& stmt) {
    stmt.kind = Statement::Kind::Select;
    expect_kw("select");
    if (accept(TokenKind::Star)) {
      stmt.select_star = true;
    } else {
      while (true) {
        stmt.select_items.push_back(parse_select_item());
        if (accept(TokenKind::Comma)) continue;
        break;
      }
    }
    if (!is_kw(peek(), "from")) {
      // SELECT without FROM is only valid for literal items (e.g. SELECT 1).
      // Anything else means the statement is still incomplete, which is how
      // the newline-terminated fallback keeps multi-line statements buffered.
      bool all_literal = !stmt.select_star && !stmt.select_items.empty();
      for (const SelectItem& item : stmt.select_items) {
        if (!item.is_literal) all_literal = false;
      }
      if (peek().kind == TokenKind::End && all_literal) {
        stmt.no_from = true;
        return;
      }
      if (peek().kind == TokenKind::End) incomplete("expected 'from'");
      syntax_error("expected 'from'");
    }
    expect_kw("from");
    stmt.table = expect_ident("a table name");
    if (accept_kw("where")) {
      stmt.has_where = true;
      stmt.where = parse_where();
    }
    if (accept_kw("order")) {
      expect_kw("by");
      while (true) {
        OrderKey key;
        if (is_kw(peek(), "l2_distance") && peek(1).kind == TokenKind::LParen) {
          // ORDER BY L2_DISTANCE(col, vec): compute it as a hidden item so the
          // projection stays unchanged.
          SelectItem item;
          item.is_distance = true;
          item.hidden = true;
          advance();
          advance();
          item.column = expect_ident("a vector column");
          expect(TokenKind::Comma, "','");
          Value query = parse_literal();
          if (query.kind != Value::Kind::Vec) {
            throw_sql_error(kErrIncorrectValue, "L2_DISTANCE query must be a vector literal");
          }
          item.query = std::move(query.v);
          expect(TokenKind::RParen, "')'");
          item.alias = "__order_distance_" + std::to_string(stmt.select_items.size());
          key.name = item.alias;
          stmt.select_items.push_back(std::move(item));
        } else {
          key.name = expect_ident("an ORDER BY column");
        }
        if (accept_kw("asc")) {
          key.asc = true;
        } else if (accept_kw("desc")) {
          key.asc = false;
        }
        stmt.order_by.push_back(std::move(key));
        if (accept(TokenKind::Comma)) continue;
        break;
      }
    }
    if (accept_kw("limit")) {
      stmt.has_limit = true;
      stmt.limit = expect_uint("a LIMIT value");
    }
  }

  SelectItem parse_select_item() {
    SelectItem item;
    if (peek().kind == TokenKind::StrLit || peek().kind == TokenKind::IntLit ||
        peek().kind == TokenKind::FloatLit || peek().kind == TokenKind::Minus ||
        peek().kind == TokenKind::Plus) {
      item.is_literal = true;
      item.literal = parse_literal();
      item.literal_text = item.literal.kind == Value::Kind::Int
                              ? std::to_string(item.literal.i)
                              : (item.literal.kind == Value::Kind::Str ? item.literal.s : "");
      if (accept_kw("as")) item.alias = expect_ident("an alias");
      return item;
    }
    if (is_kw(peek(), "l2_distance")) {
      if (peek(1).kind != TokenKind::LParen) {
        syntax_error("expected '(' after L2_DISTANCE");
      }
      advance();
      advance();
      item.is_distance = true;
      item.column = expect_ident("a vector column");
      expect(TokenKind::Comma, "','");
      Value query = parse_literal();
      if (query.kind != Value::Kind::Vec) {
        throw_sql_error(kErrIncorrectValue, "L2_DISTANCE query must be a vector literal");
      }
      item.query = std::move(query.v);
      expect(TokenKind::RParen, "')'");
    } else {
      item.column = expect_ident("a column name");
    }
    if (accept_kw("as")) {
      item.alias = expect_ident("an alias");
    } else if (peek().kind == TokenKind::Ident && !is_kw(peek(), "from")) {
      item.alias = expect_ident("an alias");
    }
    return item;
  }

  std::vector<Condition> parse_where() {
    std::vector<Condition> conditions;
    while (true) {
      if (is_kw(peek(), "or")) {
        throw_sql_error(kErrNotSupported, "OR is not supported");
      }
      if (accept(TokenKind::LParen)) {
        // Parenthesised groups only change precedence, which a flat AND chain
        // does not need; flatten them into the same list.
        std::vector<Condition> inner = parse_where();
        expect(TokenKind::RParen, "')'");
        conditions.insert(conditions.end(), inner.begin(), inner.end());
        if (accept_kw("and")) continue;
        break;
      }
      Condition cond;
      cond.column = expect_ident("a column name");
      switch (peek().kind) {
        case TokenKind::Eq: cond.op = Condition::Op::Eq; break;
        case TokenKind::Lt: cond.op = Condition::Op::Lt; break;
        case TokenKind::Le: cond.op = Condition::Op::Le; break;
        case TokenKind::Gt: cond.op = Condition::Op::Gt; break;
        case TokenKind::Ge: cond.op = Condition::Op::Ge; break;
        default: syntax_error("expected a comparison operator");
      }
      advance();
      cond.literal = parse_literal();
      conditions.push_back(std::move(cond));
      if (accept_kw("and")) continue;
      break;
    }
    return conditions;
  }

  void parse_update(Statement& stmt) {
    stmt.kind = Statement::Kind::Update;
    expect_kw("update");
    stmt.table = expect_ident("a table name");
    expect_kw("set");
    while (true) {
      std::string column = expect_ident("a column name");
      expect(TokenKind::Eq, "'='");
      Value value = parse_literal();
      stmt.set_items.emplace_back(std::move(column), std::move(value));
      if (accept(TokenKind::Comma)) continue;
      break;
    }
    if (accept_kw("where")) {
      stmt.has_where = true;
      stmt.where = parse_where();
    }
  }

  void parse_delete(Statement& stmt) {
    stmt.kind = Statement::Kind::Delete;
    expect_kw("delete");
    if (!is_kw(peek(), "from")) {
      // SELECT without FROM is only valid for literal items (e.g. SELECT 1).
      // Anything else means the statement is still incomplete, which is how
      // the newline-terminated fallback keeps multi-line statements buffered.
      bool all_literal = !stmt.select_star && !stmt.select_items.empty();
      for (const SelectItem& item : stmt.select_items) {
        if (!item.is_literal) all_literal = false;
      }
      if (peek().kind == TokenKind::End && all_literal) {
        stmt.no_from = true;
        return;
      }
      if (peek().kind == TokenKind::End) incomplete("expected 'from'");
      syntax_error("expected 'from'");
    }
    expect_kw("from");
    stmt.table = expect_ident("a table name");
    if (accept_kw("where")) {
      stmt.has_where = true;
      stmt.where = parse_where();
    }
    if (is_kw(peek(), "limit")) {
      throw_sql_error(kErrNotSupported, "LIMIT in DELETE is not supported");
    }
  }

  std::vector<Token> toks_;
  std::deque<std::string> owned_;
  size_t pos_ = 0;
};

}  // namespace

std::vector<float> parse_vector_literal(const std::string& text) {
  return parse_vector_text(text);
}

Statement parse_statement(std::string_view sql) {
  Parser parser(tokenize(sql));
  return parser.parse();
}

}  // namespace mvdb
