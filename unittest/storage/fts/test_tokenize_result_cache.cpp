/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <cassert>
#include <string>
#include "sql/engine/expr/tokenize_result_cache.h"

using oceanbase::sql::TokenizeResultCache;

int main()
{
  TokenizeResultCache cache;
  std::string_view result;
  assert(!cache.get(1, 45, "ik", "text", result));
  std::string text("a\0b", 3), value("json\0bytes", 10);
  cache.put(1, 45, "ik", text, value);
  text.assign("xxx");
  value.assign("overwrite");
  assert(cache.get(1, 45, "ik", std::string_view("a\0b", 3), result));
  assert(result == std::string_view("json\0bytes", 10));
  assert(!cache.get(1, 45, "ik", "a", result));
  assert(!cache.get(1, 45, "beng", std::string_view("a\0b", 3), result));
  assert(!cache.get(1, 46, "ik", std::string_view("a\0b", 3), result));
  assert(!cache.get(2, 45, "ik", std::string_view("a\0b", 3), result));
  assert(!cache.get(0, 45, "ik", std::string_view("a\0b", 3), result));
  cache.put(0, 45, "ik", "invalid generation", "result");
  assert(!cache.get(0, 45, "ik", "invalid generation", result));
  for (int i = 0; i < 100; ++i) {
    const std::string key = std::to_string(i);
    cache.put(2, 45, "beng", key, key);
    assert(cache.get(2, 45, "beng", key, result));
    assert(result == key);
  }
  assert(!cache.get(2, 45, "beng", "0", result));
  std::string large_text(TokenizeResultCache::MAX_TEXT_BYTES + 1, 'x');
  cache.put(2, 45, "ik", large_text, "result");
  assert(!cache.get(2, 45, "ik", large_text, result));
  std::string large_result(TokenizeResultCache::MAX_RESULT_BYTES + 1, 'x');
  cache.put(2, 45, "ik", "large result", large_result);
  assert(!cache.get(2, 45, "ik", "large result", result));
  cache.put(2, 45, "space", "", "[]");
  assert(cache.get(2, 45, "space", "", result));
  assert(result == "[]");
  // Caches belong to one worker, with no shared mutable state.
  TokenizeResultCache other;
  assert(!other.get(2, 45, "space", "", result));
}
