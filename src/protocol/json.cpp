#include "protocol/json.h"

#include <charconv>
#include <cstdlib>

namespace mvdb {

namespace {

class Parser {
 public:
  explicit Parser(std::string_view input) : input_(input) {}

  bool parse_document(JsonValue& out, std::string& error) {
    skip_ws();
    if (!parse_value(out, 0)) {
      error = error_.empty() ? "invalid JSON" : error_;
      return false;
    }
    skip_ws();
    if (pos_ != input_.size()) {
      error = "trailing characters after JSON value";
      return false;
    }
    return true;
  }

 private:
  void skip_ws() {
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool fail(const char* message) {
    if (error_.empty()) error_ = message;
    return false;
  }

  bool parse_value(JsonValue& out, int depth) {
    if (depth > 32) return fail("JSON nesting too deep");
    if (pos_ >= input_.size()) return fail("unexpected end of JSON");
    const char c = input_[pos_];
    if (c == '{') return parse_object(out, depth);
    if (c == '[') return parse_array(out, depth);
    if (c == '"') {
      out.type = JsonValue::Type::String;
      return parse_string(out.text);
    }
    if (c == 't' || c == 'f') return parse_bool(out);
    if (c == 'n') return parse_null(out);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
    return fail("unexpected character in JSON");
  }

  bool parse_object(JsonValue& out, int depth) {
    out.type = JsonValue::Type::Object;
    ++pos_;  // '{'
    skip_ws();
    if (pos_ < input_.size() && input_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      skip_ws();
      if (pos_ >= input_.size() || input_[pos_] != '"') return fail("expected object key");
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (pos_ >= input_.size() || input_[pos_] != ':') return fail("expected ':'");
      ++pos_;
      skip_ws();
      JsonValue value;
      if (!parse_value(value, depth + 1)) return false;
      out.members.emplace_back(std::move(key), std::move(value));
      skip_ws();
      if (pos_ < input_.size() && input_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < input_.size() && input_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool parse_array(JsonValue& out, int depth) {
    out.type = JsonValue::Type::Array;
    ++pos_;  // '['
    skip_ws();
    if (pos_ < input_.size() && input_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      skip_ws();
      JsonValue value;
      if (!parse_value(value, depth + 1)) return false;
      out.elements.push_back(std::move(value));
      skip_ws();
      if (pos_ < input_.size() && input_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < input_.size() && input_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parse_string(std::string& out) {
    ++pos_;  // '"'
    out.clear();
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= input_.size()) return fail("unterminated escape");
        const char esc = input_[pos_++];
        switch (esc) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (pos_ + 4 > input_.size()) return fail("truncated \\u escape");
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              const char h = input_[pos_++];
              code <<= 4;
              if (h >= '0' && h <= '9') {
                code |= static_cast<unsigned>(h - '0');
              } else if (h >= 'a' && h <= 'f') {
                code |= static_cast<unsigned>(h - 'a' + 10);
              } else if (h >= 'A' && h <= 'F') {
                code |= static_cast<unsigned>(h - 'A' + 10);
              } else {
                return fail("bad \\u escape");
              }
            }
            append_utf8(out, code);
            break;
          }
          default:
            return fail("bad escape");
        }
        continue;
      }
      out.push_back(c);
      ++pos_;
    }
    return fail("unterminated string");
  }

  static void append_utf8(std::string& out, unsigned code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  bool parse_bool(JsonValue& out) {
    if (input_.substr(pos_, 4) == "true") {
      pos_ += 4;
      out.type = JsonValue::Type::Bool;
      out.boolean = true;
      return true;
    }
    if (input_.substr(pos_, 5) == "false") {
      pos_ += 5;
      out.type = JsonValue::Type::Bool;
      out.boolean = false;
      return true;
    }
    return fail("invalid literal");
  }

  bool parse_null(JsonValue& out) {
    if (input_.substr(pos_, 4) != "null") return fail("invalid literal");
    pos_ += 4;
    out.type = JsonValue::Type::Null;
    return true;
  }

  bool parse_number(JsonValue& out) {
    const size_t start = pos_;
    if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) return fail("invalid number");
    out.type = JsonValue::Type::Number;
    out.text = std::string(input_.substr(start, pos_ - start));
    return true;
  }

  std::string_view input_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

const JsonValue* JsonValue::find(std::string_view key) const {
  for (const auto& member : members) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

bool JsonValue::as_int64(int64_t& out) const {
  if (type != Type::Number) return false;
  int64_t value = 0;
  const char* first = text.data();
  const char* last = text.data() + text.size();
  const auto result = std::from_chars(first, last, value, 10);
  if (result.ec != std::errc() || result.ptr != last) return false;
  out = value;
  return true;
}

bool parse_json(std::string_view input, JsonValue& out, std::string& error) {
  Parser parser(input);
  return parser.parse_document(out, error);
}

}  // namespace mvdb
