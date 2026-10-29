/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include "storage/fts/ik/ob_ik_char_util.h"
#include "share/text_analysis/ob_text_analyzer.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::share;
using Kind = ObFTCharUtil::CharType;

// Frozen reference of the pre-optimization classification order and decoding.
// In particular UTF16's existing decoder and surrogate behavior are retained.
template<ObCharsetType CS>
int reference_classify(const char *text, uint8_t len, Kind &kind)
{
  kind = Kind::USELESS;
  if (text == nullptr) { return OB_INVALID_ARGUMENT; }
  using Predicate = bool (*)(ob_wc_t);
  const Predicate predicates[] = {ObUnicodeBlockUtils::is_alpha, ObUnicodeBlockUtils::is_arabic,
      ObUnicodeBlockUtils::is_chinese, ObUnicodeBlockUtils::is_other_cjk,
      ObUnicodeBlockUtils::check_high_surrogate, ObUnicodeBlockUtils::check_low_surrogate};
  const Kind kinds[] = {Kind::ENGLISH_LETTER, Kind::ARABIC_LETTER, Kind::CHINESE,
      Kind::OTHER_CJK, Kind::SURROGATE_HIGH, Kind::SURROGATE_LOW};
  const int count = CS == CHARSET_UTF8MB4 ? 4 : 6;
  for (int i = 0; i < count; ++i) {
    ob_wc_t code = 0;
    const auto *begin = reinterpret_cast<const unsigned char *>(text);
    if (ob_charset_decode_unicode<CS>(begin, begin + len, code) < 0) {
      return OB_UNEXPECT_INTERNAL_ERROR;
    }
    if (predicates[i](code)) { kind = kinds[i]; break; }
  }
  return OB_SUCCESS;
}

TEST(FtsHotpath, ExhaustiveUtf8Classification)
{
  for (ob_wc_t cp = 0; cp <= 0x10ffff; ++cp) {
    unsigned char buffer[8] = {};
    const int len = ob_charset_encode_unicode<CHARSET_UTF8MB4>(cp, buffer, buffer + sizeof(buffer));
    if (len <= 0) { continue; }
    Kind expected, actual;
    const char *text = reinterpret_cast<const char *>(buffer);
    const int reference = reference_classify<CHARSET_UTF8MB4>(text, len, expected);
    ASSERT_EQ(reference, ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, text, len, actual)) << cp;
    ASSERT_EQ(expected, actual) << cp;
  }
}

TEST(FtsHotpath, Utf16CodeUnitsAndTruncatedUtf8)
{
  for (unsigned cp = 0; cp <= 0xffff; ++cp) {
    if (ObCharset::charset_type_by_coll(CS_TYPE_UTF16_BIN) != CHARSET_UTF16) {
      Kind kind;
      ASSERT_EQ(OB_NOT_SUPPORTED, ObFTCharUtil::classify_first_char(CS_TYPE_UTF16_BIN, "a", 1, kind));
      break; // This build deliberately omits UTF16 charset support.
    }
    const char text[] = {static_cast<char>(cp >> 8), static_cast<char>(cp),
                         static_cast<char>(cp), static_cast<char>(cp >> 8)};
    Kind expected, actual;
    ASSERT_EQ(reference_classify<CHARSET_UTF16>(text, 2, expected),
        ObFTCharUtil::classify_first_char(CS_TYPE_UTF16_BIN, text, 2, actual));
    ASSERT_EQ(expected, actual);
    ASSERT_EQ(reference_classify<CHARSET_UTF16LE>(text + 2, 2, expected),
        ObFTCharUtil::classify_first_char(CS_TYPE_UTF16LE_BIN, text + 2, 2, actual));
    ASSERT_EQ(expected, actual);
  }
  for (const std::string &text : {std::string("\xc2"), std::string("\xe4\xb8"),
       std::string("\xf0\x9f\x98"), std::string("\xff"), std::string("\xc0\xaf")}) {
    Kind expected, actual;
    EXPECT_EQ(reference_classify<CHARSET_UTF8MB4>(text.data(), text.size(), expected),
        ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, text.data(), text.size(), actual));
    EXPECT_EQ(expected, actual);
  }
}

static std::vector<std::string> analyze(ObEnglishTextAnalyzer &analyzer, const std::string &text)
{
  ObDatum doc;
  doc.set_string(text.data(), text.size());
  ObITokenStream *stream = nullptr;
  EXPECT_EQ(OB_SUCCESS, analyzer.analyze(doc, stream));
  std::vector<std::string> words;
  if (stream != nullptr) {
    ObDatum token;
    int64_t freq = 0;
    int ret;
    while ((ret = stream->get_next(token, freq)) == OB_SUCCESS) {
      EXPECT_EQ(1, freq);
      words.emplace_back(token.ptr_, static_cast<uint32_t>(token.len_));
    }
    EXPECT_EQ(OB_ITER_END, ret);
  }
  return words;
}

TEST(FtsHotpath, EnglishReuseAndNormalization)
{
  ObArenaAllocator allocator;
  ObTextAnalysisCtx ctx;
  ctx.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  ctx.filter_stopword_ = false;
  ctx.need_grouping_ = false;
  ObEnglishTextAnalyzer analyzer;
  ASSERT_EQ(OB_SUCCESS, analyzer.init(ctx, allocator));
  EXPECT_EQ((std::vector<std::string>{"hello", "world", "123", "abc", "def"}),
      analyze(analyzer, "HELLO,world! 123 abc_def"));
  EXPECT_EQ((std::vector<std::string>{"lower", "again"}), analyze(analyzer, "lower AGAIN"));
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), analyze(analyzer, std::string("a\0b\tc", 5)));
  EXPECT_TRUE(analyze(analyzer, " \t\n---").empty());
}

TEST(FtsHotpath, WhitespaceAllBytePairs)
{
  const auto *cs = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  // The reference follows the original byte classification + charset-length
  // stepping, including the partial token emitted before an invalid character.
  for (unsigned pair = 0; pair < 65536; ++pair) {
    // Legacy length stepping can extend past a truncated head byte.
    // Supply readable padding while retaining a four-byte logical document.
    const char bytes[8] = {'a', static_cast<char>(pair >> 8), static_cast<char>(pair), 'z'};
    constexpr size_t document_length = 4;
    std::vector<std::string> expected, actual;
    size_t pos = 0;
    while (pos < document_length) {
      auto delimiter = [&]() { return ob_isspace(cs, bytes[pos]) || ob_iscntrl(cs, bytes[pos]) || ob_ispunct(cs, bytes[pos]); };
      while (pos < document_length && delimiter()) {
        int len = ob_mbcharlen_ptr(cs, bytes + pos, bytes + document_length);
        if (len == 0) { pos = document_length; break; }
        pos += len;
      }
      size_t begin = pos, length = 0;
      while (pos < document_length && !delimiter()) {
        int len = ob_mbcharlen_ptr(cs, bytes + pos, bytes + document_length);
        if (len == 0) { pos = document_length; break; }
        pos += len;
        length += len;
      }
      ASSERT_LE(begin + length, sizeof(bytes)) << pair;
      if (length > 0) { expected.emplace_back(bytes + begin, length); }
    }
    ObDatum doc;
    doc.set_string(bytes, document_length);
    ObTextWhitespaceTokenizer tokenizer;
    ASSERT_EQ(OB_SUCCESS, tokenizer.open(doc, cs));
    ObDatum token;
    int64_t frequency;
    int ret;
    while ((ret = tokenizer.get_next(token, frequency)) == OB_SUCCESS) {
      actual.emplace_back(token.ptr_, static_cast<uint32_t>(token.len_));
      ASSERT_EQ(1, frequency);
    }
    ASSERT_EQ(OB_ITER_END, ret);
    ASSERT_EQ(expected, actual) << pair;
  }
}

TEST(FtsHotpath, Microbenchmark)
{
  if (std::getenv("FTS_MICROBENCH") == nullptr) { return; }
  using Clock = std::chrono::steady_clock;
  const std::vector<std::string> chars = {"数", "据", "库", "。", "A", "5", "あ", "😀"};
  uint64_t checksum = 0;
  auto start = Clock::now();
  for (int i = 0; i < 10000000; ++i) {
    for (const auto &text : chars) {
      Kind kind;
      const int ret = ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, text.data(), text.size(), kind);
      checksum += static_cast<unsigned>(kind) + static_cast<unsigned>(ret);
    }
  }
  const double classify_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  ObArenaAllocator allocator;
  ObTextAnalysisCtx ctx;
  ctx.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  ctx.filter_stopword_ = false;
  ctx.need_grouping_ = false;
  ObEnglishTextAnalyzer analyzer;
  ASSERT_EQ(OB_SUCCESS, analyzer.init(ctx, allocator));
  const std::string text = "Text analysis has repeated Words, numbers 12345 and punctuation. ";
  ObDatum doc;
  doc.set_string(text.data(), text.size());
  start = Clock::now();
  for (int i = 0; i < 1000000; ++i) {
    ObITokenStream *stream = nullptr;
    ASSERT_EQ(OB_SUCCESS, analyzer.analyze(doc, stream));
    ObDatum word;
    int64_t freq;
    while (stream->get_next(word, freq) == OB_SUCCESS) { checksum += word.len_; }
  }
  const double english_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  std::printf("FTS_MICROBENCH classify_ms=%.3f english_ms=%.3f checksum=%lu\n", classify_ms, english_ms, checksum);
}
int main(int argc, char **argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
