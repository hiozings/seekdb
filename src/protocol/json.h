#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mvdb {

// Minimal JSON reader for protocol requests. Numbers keep their raw text so a
// request id can be echoed back with exactly the same JSON type and digits.
class JsonValue {
 public:
  enum class Type { Null, Bool, Number, String, Object, Array };

  Type type = Type::Null;
  bool boolean = false;
  std::string text;  // String contents, or the raw literal for Number
  std::vector<std::pair<std::string, JsonValue>> members;
  std::vector<JsonValue> elements;

  const JsonValue* find(std::string_view key) const;
  bool is_string() const { return type == Type::String; }
  bool is_number() const { return type == Type::Number; }
  bool as_int64(int64_t& out) const;
};

// Parses one complete JSON document. Returns false and fills `error` on
// malformed input.
bool parse_json(std::string_view input, JsonValue& out, std::string& error);

}  // namespace mvdb
