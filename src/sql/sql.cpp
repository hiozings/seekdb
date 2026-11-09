#include "sql/sql.h"

#include <cmath>
#include <cctype>
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

struct Token {
  TokenKind kind = TokenKind::End;
  std::string text;
  size_t pos = 0;
};

[[noreturn]] void syntax_error(const std::string& detail) {
  throw_sql_error(kErrSyntax, "syntax error: " + detail);
}

[[noreturn]] void incomplete(const std::string& detail) {
  throw SqlError{kIncomplete, detail};
}

std::vector<Token> tokenize(const std::string& sql) {
  std::vector<Token> out;
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
      size_t start = i;
      ++i;
      std::string value;
      bool closed = false;
      while (i < n) {
        if (sql[i] == '\'') {
          if (i + 1 < n && sql[i + 1] == '\'') {
            value.push_back('\'');
            i += 2;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        value.push_back(sql[i]);
        ++i;
      }
      if (!closed) syntax_error("unterminated string literal");
      (void)start;
      out.push_back(Token{TokenKind::StrLit, value, start});
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
  out.push_back(Token{TokenKind::End, "", n});
  return out;
}

// Converts a double to float, rejecting overflow and non-finite values.
float to_float32(double value, const char* what) {
  if (!std::isfinite(value)) {
    throw_sql_error(kErrIncorrectValue, std::string("non-finite value in ") + what);
  }
  float f = static_cast<float>(value);
  if (!std::isfinite(f)) {
    throw_sql_error(kErrOutOfRange, std::string("value out of float32 range in ") + what);
  }
  return f;
}

std::vector<float> parse_vector_text(const std::string& text) {
  std::string inner = trim(text);
  if (inner.size() < 2 || inner.front() != '[' || inner.back() != ']') {
    throw_sql_error(kErrIncorrectValue, "vector literal must be [v1, v2, ...]");
  }
  inner = inner.substr(1, inner.size() - 2);
  std::vector<float> out;
  if (trim(inner).empty()) return out;
  for (const std::string& part : split(inner, ',')) {
    double d = 0.0;
    if (!parse_double(part, d)) {
      throw_sql_error(kErrIncorrectValue, "invalid number in vector literal: " + trim(part));
    }
    out.push_back(to_float32(d, "vector literal"));
  }
  return out;
}

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

  Statement parse() {
    Statement stmt;
    if (peek().kind == TokenKind::End) incomplete("empty statement");
    if (peek().kind != TokenKind::Ident) syntax_error("expected a statement keyword");
    const std::string kw = to_lower(peek().text);
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
      throw_sql_error(kErrSyntax, "syntax error: unsupported statement '" + peek().text + "'");
    }
    if (peek().kind != TokenKind::End) {
      syntax_error("unexpected trailing token '" + peek().text + "'");
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
    return tok.kind == TokenKind::Ident && iequals(tok.text, kw);
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
    std::string name = peek().text;
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
        throw_sql_error(kErrOutOfRange, "invalid integer literal '" + peek().text + "'");
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
        throw_sql_error(kErrOutOfRange, "invalid float literal '" + peek().text + "'");
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
      std::string text = peek().text;
      advance();
      std::string trimmed = trim(text);
      if (!trimmed.empty() && trimmed.front() == '[') {
        return Value::make_vec(parse_vector_text(text));
      }
      return Value::make_str(std::move(text));
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
        key.name = expect_ident("an ORDER BY column");
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
  size_t pos_ = 0;
};

}  // namespace

std::vector<float> parse_vector_literal(const std::string& text) {
  return parse_vector_text(text);
}

Statement parse_statement(const std::string& sql) {
  std::vector<Token> tokens = tokenize(sql);
  Parser parser(std::move(tokens));
  return parser.parse();
}

}  // namespace mvdb
