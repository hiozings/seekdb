/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdint>
#include <array>
#include <cstring>
#include <string_view>

namespace oceanbase { namespace sql {

class TokenizeResultCache final
{
public:
  static constexpr int64_t MAX_TEXT_BYTES = 4096;
  static constexpr int64_t MAX_RESULT_BYTES = 16384;
  static constexpr int64_t MAX_PARSER_BYTES = 64;
  // Only the default-property SQL form is eligible. All argument expressions
  // are still evaluated on a hit. No hash is trusted as an input identity.
  bool get(uint64_t epoch, int64_t collation, std::string_view parser,
           std::string_view text, std::string_view &result) const
  {
    bool found = false;
    result = {};
    if (epoch != 0 && fits(parser, text)) {
      for (const Entry &entry : entries_) {
        if (entry.epoch_ == epoch && entry.collation_ == collation
            && std::string_view(entry.parser_.data(), entry.parser_len_) == parser
            && std::string_view(entry.text_.data(), entry.text_len_) == text) {
          result = std::string_view(entry.result_.data(), entry.result_len_);
          found = true;
          break;
        }
      }
    }
    return found;
  }

  void put(uint64_t epoch, int64_t collation, std::string_view parser,
           std::string_view text, std::string_view result)
  {
    if (epoch != 0 && fits(parser, text) && result.size() <= MAX_RESULT_BYTES) {
      Entry &entry = entries_[next_];
      entry.epoch_ = 0;
      if (!parser.empty()) { std::memcpy(entry.parser_.data(), parser.data(), parser.size()); }
      if (!text.empty()) { std::memcpy(entry.text_.data(), text.data(), text.size()); }
      if (!result.empty()) { std::memcpy(entry.result_.data(), result.data(), result.size()); }
      entry.parser_len_ = parser.size();
      entry.text_len_ = text.size();
      entry.result_len_ = result.size();
      entry.collation_ = collation;
      entry.epoch_ = epoch;
      next_ = (next_ + 1) % entries_.size();
    }
  }

private:
  static bool fits(std::string_view parser, std::string_view text)
  { return parser.size() <= MAX_PARSER_BYTES && text.size() <= MAX_TEXT_BYTES; }
  struct Entry
  {
    uint64_t epoch_ = 0;
    int64_t collation_ = 0;
    size_t parser_len_ = 0;
    size_t text_len_ = 0;
    size_t result_len_ = 0;
    std::array<char, MAX_PARSER_BYTES> parser_{};
    std::array<char, MAX_TEXT_BYTES> text_{};
    std::array<char, MAX_RESULT_BYTES> result_{};
  };
  // A worker owns its two entries (about 41 KiB); eviction cannot grow memory.
  std::array<Entry, 2> entries_{};
  size_t next_ = 0;
};

} }
