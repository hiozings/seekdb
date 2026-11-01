/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "share/ob_rpc_struct.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ob_fts_compact_posting.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/fts_token_cache.h"
#include "storage/blocksstable/encoding/ob_string_diff_decoder.h"
#include "storage/blocksstable/encoding/ob_bit_stream.h"
#include "storage/blocksstable/encoding/ob_row_index.h"
#include "storage/blocksstable/encoding/ob_micro_block_encoder.h"
#include "storage/blocksstable/encoding/ob_raw_decoder.h"
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
TEST(FtsHotpath, CompactPostingCodec)
{
  using namespace oceanbase::storage;
  ObArenaAllocator allocator;
  ObSEArray<ObFtsCompactPosting, 8> a, b, c;
  char id1[16] = {1};
  char id2[16] = {2};
  char id3[16] = {3};
  ASSERT_EQ(OB_SUCCESS, a.push_back(ObFtsCompactPosting(id1, 2, 10)));
  ASSERT_EQ(OB_SUCCESS, a.push_back(ObFtsCompactPosting(id3, 1, 10)));
  ASSERT_EQ(OB_SUCCESS, b.push_back(ObFtsCompactPosting(id2, 4, 20)));
  ASSERT_EQ(OB_SUCCESS, b.push_back(ObFtsCompactPosting(id3, 2, 20)));
  ASSERT_EQ(OB_SUCCESS, c.push_back(ObFtsCompactPosting(id1, 1, 5)));
  ObString ba, bb, bc;
  ASSERT_EQ(OB_SUCCESS, ObFtsCompactPostingCodec::encode(a, allocator, ba));
  ASSERT_EQ(OB_SUCCESS, ObFtsCompactPostingCodec::encode(b, allocator, bb));
  ASSERT_EQ(OB_SUCCESS, ObFtsCompactPostingCodec::encode(c, allocator, bc));
  ASSERT_EQ(2 * ObFtsCompactPostingCodec::POSTING_SIZE, ba.length());
  ObSEArray<ObFtsCompactPosting, 8> decoded;
  ASSERT_EQ(OB_SUCCESS, ObFtsCompactPostingCodec::decode(ba, decoded));
  ASSERT_EQ(a.count(), decoded.count());
  for (int64_t i = 0; i < a.count(); ++i) {
    ASSERT_EQ(0, memcmp(a.at(i).doc_id_, decoded.at(i).doc_id_, ObFtsCompactPosting::DOC_ID_SIZE));
    ASSERT_EQ(a.at(i).tf_, decoded.at(i).tf_);
    ASSERT_EQ(a.at(i).doc_len_, decoded.at(i).doc_len_);
  }
  ObSEArray<const ObString *, 4> blobs;
  ASSERT_EQ(OB_SUCCESS, blobs.push_back(&ba));
  ASSERT_EQ(OB_SUCCESS, blobs.push_back(&bb));
  ASSERT_EQ(OB_SUCCESS, blobs.push_back(&bc));
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, ObFtsCompactPostingCodec::union_count(blobs, count));
  ASSERT_EQ(3, count); // {1,3} U {2,3} U {1} = {1,2,3}
}
TEST(FtsHotpath, EmptyParserPropertiesMatchDdlDefaults)
{
  for (const char *name : {"ik", "beng", "space", "ngram", "ngram2"}) {
    SCOPED_TRACE(name);
    ObFTParser parser;
    ASSERT_EQ(OB_SUCCESS, parser.init(ObString::make_string(name)));
    char inner_name[OB_FT_PARSER_NAME_LENGTH] = {};
    ASSERT_EQ(OB_SUCCESS, parser.serialize_to_str(inner_name, sizeof(inner_name)));
    ObFTParserJsonProps props;
    ASSERT_EQ(OB_SUCCESS, props.init());
    ASSERT_EQ(OB_SUCCESS, props.rebuild_props_for_ddl(
        ObString::make_string(inner_name), CS_TYPE_UTF8MB4_BIN, false));
    ObArenaAllocator alloc;
    ObString json;
    ASSERT_EQ(OB_SUCCESS, props.to_format_json(alloc, json));
    ObFTParserProperty expected, actual;
    ASSERT_EQ(OB_SUCCESS, expected.parse_for_parser_helper(parser, json));
    // Reusing a property must not retain an earlier parser's settings.
    actual.min_token_size_ = 19;
    actual.ik_mode_smart_ = false;
    ASSERT_EQ(OB_SUCCESS, actual.parse_for_parser_helper(parser, ObString()));
    EXPECT_TRUE(expected.is_equal(actual));
  }
}

TEST(FtsHotpath, StopwordLookupPreservesCollation)
{
  ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  for (ObCollationType coll : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
                               CS_TYPE_LATIN1_SWEDISH_CI}) {
    SCOPED_TRACE(static_cast<int>(coll));
    if (!ObCharset::is_valid_collation(coll)) { continue; }
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(coll);
    for (const char *word : {"the", "THE", "database", "tokenizer"}) {
      bool found = false;
      ASSERT_EQ(OB_SUCCESS, checker.check_stopword(
          ObFTWord(strlen(word), word, meta), found));
      EXPECT_EQ(0 == strcmp(word, "the") || 0 == strcmp(word, "THE"), found);
    }
  }
}

TEST(FtsHotpath, RepeatedWordsRetainOriginalKeyAndFrequency)
{
  ObArenaAllocator alloc;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(17, ObMemAttr("FTWordMap")));
  ObFTParserProperty props;
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ObAddWordFlag flags;
  flags.set_groupby_word();
  ObAddWord add(props, meta, flags, alloc, words);
  ASSERT_EQ(OB_SUCCESS, add.process_word("Alpha", 5, 5, 1));
  ASSERT_EQ(OB_SUCCESS, add.process_word("alpha", 5, 5, 1));
  ASSERT_EQ(OB_SUCCESS, add.process_word("Beta", 4, 4, 1));
  ASSERT_EQ(2, words.size());
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(5, "ALPHA", meta), count));
  EXPECT_EQ(2, count);
  EXPECT_EQ(3, add.get_add_word_count());
}

TEST(FtsHotpath, SegmentationCacheOwnsBytesAndPreservesOrder)
{
  FTTokenCache cache;
  ASSERT_EQ(OB_SUCCESS, cache.init());
  ObFTParser parser;
  ASSERT_EQ(OB_SUCCESS, parser.init(ObString::make_string("ik")));
  ObFTParserProperty properties;
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  FTTokenCacheKey key;
  key.text_hash_ = 42;
  key.text_len_ = 8;
  key.collation_ = CS_TYPE_UTF8MB4_BIN;
  std::vector<std::pair<std::string, int64_t>> expected;
  {
    ObFTWordMap source;
    ASSERT_EQ(OB_SUCCESS, source.create(2, ObMemAttr("FTCacheTest")));
    key.word_bucket_count_ = source.bucket_count();
    std::vector<std::string> text;
    for (int i = 0; text.size() < 6 && i < 100000; ++i) {
      std::string value = "term" + std::to_string(i);
      uint64_t hash = 0;
      ASSERT_EQ(OB_SUCCESS, ObFTWord(value.size(), value.data(), meta).hash(hash));
      if (hash % source.bucket_count() == 0) { text.push_back(value); }
    }
    ASSERT_EQ(6, text.size()); // Force a nontrivial bucket chain.
    for (size_t i = 0; i < text.size(); ++i) {
      ASSERT_EQ(OB_SUCCESS, source.set_refactored(ObFTWord(text[i].size(), text[i].data(), meta), i + 1));
    }
    for (const auto &word : source) {
      const auto bytes = word.first.get_word().get_string();
      expected.emplace_back(std::string(bytes.ptr(), bytes.length()), word.second);
    }
    ASSERT_EQ(OB_SUCCESS, cache.try_store(key, parser.get_parser_name(), parser.get_parser_version(),
        properties, "document", 8, 21, source));
  }
  ObArenaAllocator alloc;
  ObFTWordMap replay;
  ASSERT_EQ(OB_SUCCESS, replay.create(2, ObMemAttr("FTCacheTest")));
  int64_t length = -1;
  bool found = false;
  ASSERT_EQ(OB_SUCCESS, cache.try_get(key, parser.get_parser_name(), parser.get_parser_version(),
      properties, "document", 8, alloc, replay, length, found));
  ASSERT_TRUE(found);
  EXPECT_EQ(21, length);
  std::vector<std::pair<std::string, int64_t>> actual;
  for (const auto &word : replay) {
    const auto bytes = word.first.get_word().get_string();
    actual.emplace_back(std::string(bytes.ptr(), bytes.length()), word.second);
  }
  EXPECT_EQ(expected, actual);
  ASSERT_EQ(OB_SUCCESS, replay.reuse());
  // A deliberately colliding digest must not return another text's tokens.
  ASSERT_EQ(OB_SUCCESS, cache.try_get(key, parser.get_parser_name(), parser.get_parser_version(),
      properties, "different", 9, alloc, replay, length, found));
  EXPECT_FALSE(found);
  auto changed = properties;
  changed.ik_mode_smart_ = !properties.ik_mode_smart_;
  ASSERT_EQ(OB_SUCCESS, cache.try_get(key, parser.get_parser_name(), parser.get_parser_version(),
      changed, "document", 8, alloc, replay, length, found));
  EXPECT_FALSE(found);
  struct LimitedReplayAllocator final : ObWrapperAllocator {
    explicit LimitedReplayAllocator(ObIAllocator &allocator) : ObWrapperAllocator(allocator) {}
    void *alloc(int64_t size) override
    { return remaining_-- > 0 ? ObWrapperAllocator::alloc(size) : nullptr; }
    int remaining_ = 2;
  } failing(alloc);
  EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED,
      cache.try_get(key, parser.get_parser_name(), parser.get_parser_version(), properties,
                    "document", 8, failing, replay, length, found));
  EXPECT_FALSE(found);
  EXPECT_EQ(2, replay.size());
  ASSERT_EQ(OB_SUCCESS, replay.reuse());
  ASSERT_EQ(OB_SUCCESS, cache.try_get(key, parser.get_parser_name(), parser.get_parser_version(),
      properties, "document", 8, alloc, replay, length, found));
  EXPECT_TRUE(found); // Failed replay did not damage the shared entry.
  std::string oversized(FTTokenCache::MAX_ENTRY_BYTES + 1, 'x');
  auto large_key = key;
  large_key.text_len_ = oversized.size();
  large_key.text_hash_ = murmurhash(oversized.data(), oversized.size(), 0);
  ASSERT_EQ(OB_SUCCESS, cache.try_store(large_key, parser.get_parser_name(), parser.get_parser_version(),
      properties, oversized.data(), oversized.size(), 21, replay));
  ObFTWordMap probe;
  ASSERT_EQ(OB_SUCCESS, probe.create(2, ObMemAttr("FTCacheTest")));
  ASSERT_EQ(OB_SUCCESS, cache.try_get(large_key, parser.get_parser_name(), parser.get_parser_version(),
      properties, oversized.data(), oversized.size(), alloc, probe, length, found));
  EXPECT_FALSE(found);
  uint64_t target_hash = 0;
  key.hash(target_hash);
  FTTokenCacheKey last_key;
  std::string last_text;
  int stored = 0;
  for (int i = 0; stored <= FTTokenCache::BUCKET_ENTRY_BUDGET && i < 1000000; ++i) {
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%012d", i);
    auto next = key;
    next.text_len_ = 12;
    next.text_hash_ = murmurhash(buffer, 12, 0);
    uint64_t hash = 0;
    next.hash(hash);
    if (hash % FTTokenCache::BUCKET_CNT != target_hash % FTTokenCache::BUCKET_CNT) { continue; }
    ASSERT_EQ(OB_SUCCESS, cache.try_store(next, parser.get_parser_name(), parser.get_parser_version(),
        properties, buffer, 12, 21, replay));
    last_key = next;
    last_text.assign(buffer, 12);
    ++stored;
  }
  ASSERT_GT(stored, FTTokenCache::BUCKET_ENTRY_BUDGET);
  ASSERT_EQ(OB_SUCCESS, cache.try_get(key, parser.get_parser_name(), parser.get_parser_version(),
      properties, "document", 8, alloc, probe, length, found));
  EXPECT_FALSE(found); // The full shard was evicted before exceeding its entry budget.
  ASSERT_EQ(OB_SUCCESS, cache.try_get(last_key, parser.get_parser_name(), parser.get_parser_version(),
      properties, last_text.data(), last_text.size(), alloc, probe, length, found));
  EXPECT_TRUE(found);
}

TEST(FtsHotpath, SegmentationCacheConcurrentReadersAndWriters)
{
  FTTokenCache cache;
  ASSERT_EQ(OB_SUCCESS, cache.init());
  std::atomic<int> errors{0};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker]() {
      ObFTParser parser;
      ObFTParserProperty properties;
      ObObjMeta meta;
      meta.set_varchar();
      meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      ObFTWordMap words, replay;
      ObArenaAllocator allocator;
      if (OB_SUCCESS != parser.init(ObString::make_string("ik"))
          || OB_SUCCESS != words.create(17, ObMemAttr("FTCacheTest"))
          || OB_SUCCESS != replay.create(17, ObMemAttr("FTCacheTest"))) {
        ++errors;
        return;
      }
      for (int i = 0; i < 300; ++i) {
        const std::string text = "document " + std::to_string((i + worker) % 100);
        const int64_t frequency = (i + worker) % 100 + 1;
        FTTokenCacheKey key;
        key.text_hash_ = murmurhash(text.data(), text.size(), 0);
        key.text_len_ = text.size();
        key.collation_ = CS_TYPE_UTF8MB4_BIN;
        key.word_bucket_count_ = words.bucket_count();
        const ObFTWord word(5, "token", meta);
        bool found = false;
        int64_t length = 0, actual = 0;
        if (OB_SUCCESS != words.set_refactored(word, frequency, 1)
            || OB_SUCCESS != cache.try_store(key, parser.get_parser_name(), parser.get_parser_version(),
                properties, text.data(), text.size(), frequency, words)
            || OB_SUCCESS != cache.try_get(key, parser.get_parser_name(), parser.get_parser_version(),
                properties, text.data(), text.size(), allocator, replay, length, found)
            || !found || length != frequency
            || OB_SUCCESS != replay.get_refactored(word, actual) || actual != frequency) {
          ++errors;
        }
        replay.reuse();
        allocator.reuse();
      }
    });
  }
  for (auto &worker : workers) { worker.join(); }
  EXPECT_EQ(0, errors.load());
}

TEST(FtsHotpath, StringDiffBatchUsesActualWidth)
{
  using namespace oceanbase::blocksstable;
  constexpr int rows = 3;
  constexpr int width = 16;
  const int meta_size = sizeof(ObStringDiffHeader) + 2 * sizeof(ObStringDiffHeader::DiffDesc) + 12;
  std::vector<char> encoded(meta_size + rows * 4, 0);
  auto *header = new (encoded.data()) ObStringDiffHeader();
  header->string_size_ = width;
  header->length_ = 4;
  header->diff_desc_cnt_ = 2;
  header->diff_descs_[0].diff_ = 0;
  header->diff_descs_[0].count_ = 12;
  header->diff_descs_[1].diff_ = 1;
  header->diff_descs_[1].count_ = 4;
  memcpy(header->common_data(), "ABCDEFGHIJKL", 12);
  const char suffixes[rows][4] = {{0, 0, 0, 1}, {0, 0, 2, 0}, {0, 3, 0, 0}};
  memcpy(encoded.data() + meta_size, suffixes, sizeof(suffixes));
  ObMicroBlockHeader micro;
  micro.row_count_ = rows;
  ObColumnHeader column;
  column.type_ = ObColumnHeader::STRING_DIFF;
  column.length_ = meta_size;
  column.set_fix_lenght_attr();
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_BINARY);
  ObArenaAllocator arena;
  struct WidthBoundAllocator final : ObWrapperAllocator {
    explicit WidthBoundAllocator(ObIAllocator &alloc) : ObWrapperAllocator(alloc) {}
    void *alloc(int64_t size) override
    { return size <= rows * width ? ObWrapperAllocator::alloc(size) : nullptr; }
  } bounded(arena);
  ObColumnDecoderCtx context;
  context.fill(meta, &micro, &column, &bounded, 0);
  ObStringDiffDecoder decoder;
  ASSERT_EQ(OB_SUCCESS, decoder.init(micro, column, encoded.data()));
  const int32_t row_ids[rows] = {2, 0, 1};
  ObDatum decoded[rows];
  ASSERT_EQ(OB_SUCCESS, decoder.batch_decode(context, nullptr, row_ids, nullptr, rows, decoded));
  for (int i = 0; i < rows; ++i) {
    ASSERT_EQ(width, decoded[i].len_);
    EXPECT_EQ(0, memcmp(decoded[i].ptr_, "ABCDEFGHIJKL", 12));
    EXPECT_EQ(0, memcmp(decoded[i].ptr_ + 12, suffixes[row_ids[i]], 4));
  }
  EXPECT_EQ(OB_SUCCESS, decoder.batch_decode(context, nullptr, nullptr, nullptr, 0, nullptr));
}

TEST(FtsHotpath, StringDiffBatchHexAndNulls)
{
  using namespace oceanbase::blocksstable;
  for (bool hex : {false, true}) {
   for (bool fixed : {false, true}) {
    const int meta_size = sizeof(ObStringDiffHeader) + 2 + (hex ? 16 : 0) + 12;
    const int cell_size = hex ? 2 : 4;
    std::vector<char> encoded(meta_size + (fixed ? 1 + 3 * cell_size : 3 * (1 + cell_size)), 0);
    auto *header = new (encoded.data()) ObStringDiffHeader();
    header->string_size_ = 16;
    header->length_ = cell_size;
    header->offset_ = fixed ? 0 : 1;
    header->diff_desc_cnt_ = 2;
    header->hex_char_array_size_ = hex ? 16 : 0;
    header->diff_descs_[0].diff_ = 0;
    header->diff_descs_[0].count_ = 12;
    header->diff_descs_[1].diff_ = 1;
    header->diff_descs_[1].count_ = 4;
    if (hex) { memcpy(header->hex_char_array(), "0123456789abcdef", 16); }
    memcpy(header->common_data(), "ABCDEFGHIJKL", 12);
    auto *data = reinterpret_cast<unsigned char *>(encoded.data() + meta_size);
    ObBitStream nulls(fixed ? data : data + 1 + cell_size, 1);
    ASSERT_EQ(OB_SUCCESS, nulls.set(fixed ? 1 : 0, 1, int64_t(1)));
    const char suffixes[3][5] = {"0123", "fedc", "a5b9"};
    const unsigned char packed[3][2] = {{0x01, 0x23}, {0xfe, 0xdc}, {0xa5, 0xb9}};
    for (int row = 0; row < 3; ++row) {
      unsigned char *cell = fixed ? data + 1 + row * cell_size : data + row * (1 + cell_size) + 1;
      if (hex) { memcpy(cell, packed[row], cell_size); }
      else { memcpy(cell, suffixes[row], cell_size); }
    }
    ObMicroBlockHeader micro;
    micro.row_count_ = 3;
    micro.extend_value_bit_ = 1;
    micro.var_column_count_ = fixed ? 0 : 1;
    ObColumnHeader column;
    column.type_ = ObColumnHeader::STRING_DIFF;
    column.length_ = meta_size;
    if (fixed) { column.set_fix_lenght_attr(); }
    column.set_has_extend_value_attr();
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(CS_TYPE_BINARY);
    ObArenaAllocator arena;
    ObColumnDecoderCtx context;
    context.fill(meta, &micro, &column, &arena, 0);
    ObStringDiffDecoder decoder;
    ASSERT_EQ(OB_SUCCESS, decoder.init(micro, column, encoded.data()));
    ObFixRowIndex row_index;
    if (!fixed) {
      ASSERT_EQ(OB_SUCCESS, row_index.init(reinterpret_cast<char *>(data), 3 * (1 + cell_size), 3));
    }
    const int32_t row_ids[] = {2, 1, 0, 2};
    ObDatum decoded[4];
    ASSERT_EQ(OB_SUCCESS, decoder.batch_decode(context, fixed ? nullptr : &row_index, row_ids, nullptr, 4, decoded));
    for (int i = 0; i < 4; ++i) {
      if (row_ids[i] == 1) { EXPECT_TRUE(decoded[i].is_null()); }
      else {
        ASSERT_EQ(16, decoded[i].len_);
        EXPECT_EQ(0, memcmp(decoded[i].ptr_, "ABCDEFGHIJKL", 12));
        EXPECT_EQ(0, memcmp(decoded[i].ptr_ + 12, suffixes[row_ids[i]], 4));
      }
    }
   }
  }
}

TEST(FtsHotpath, SmallDiffCopiesPreserveBounds)
{
  using oceanbase::blocksstable::ObStringDiffHeader;
  char source[512], actual[512], expected[512];
  for (int i = 0; i < 512; ++i) { source[i] = static_cast<char>(i * 31); }
  for (int offset = 0; offset < 8; ++offset) {
    for (int size = 0; size <= 256; ++size) {
      memset(actual, 0x5a, sizeof(actual));
      memset(expected, 0x5a, sizeof(expected));
      memcpy(expected + 3, source + offset, size);
      ObStringDiffHeader::LeftToRight()(source + offset, actual + 3, size);
      EXPECT_EQ(0, memcmp(expected, actual, sizeof(actual))) << offset << ":" << size;
    }
  }
}

TEST(FtsHotpath, StringDiffManySpansFallback)
{
  using namespace oceanbase::blocksstable;
  constexpr int width = 35; // 18 differing spans: exceeds the fast-path span array.
  constexpr int common_count = 17;
  constexpr int diff_count = 18;
  const int meta_size = sizeof(ObStringDiffHeader) + width + common_count;
  std::vector<char> encoded(meta_size + 2 * diff_count, 0);
  auto *header = new (encoded.data()) ObStringDiffHeader();
  header->string_size_ = width;
  header->length_ = diff_count;
  header->diff_desc_cnt_ = width;
  for (int i = 0; i < width; ++i) {
    header->diff_descs_[i].diff_ = (i % 2 == 0);
    header->diff_descs_[i].count_ = 1;
  }
  for (int i = 0; i < common_count; ++i) { header->common_data()[i] = static_cast<char>(0x80 + i); }
  for (int row = 0; row < 2; ++row) {
    for (int i = 0; i < diff_count; ++i) { encoded[meta_size + row * diff_count + i] = row * 64 + i; }
  }
  ObMicroBlockHeader micro;
  micro.row_count_ = 2;
  ObColumnHeader column;
  column.type_ = ObColumnHeader::STRING_DIFF;
  column.length_ = meta_size;
  column.set_fix_lenght_attr();
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_BINARY);
  ObArenaAllocator allocator;
  ObColumnDecoderCtx context;
  context.fill(meta, &micro, &column, &allocator, 0);
  ObStringDiffDecoder decoder;
  ASSERT_EQ(OB_SUCCESS, decoder.init(micro, column, encoded.data()));
  const int32_t row_ids[] = {1, 0};
  ObDatum decoded[2];
  ASSERT_EQ(OB_SUCCESS, decoder.batch_decode(context, nullptr, row_ids, nullptr, 2, decoded));
  for (int row = 0; row < 2; ++row) {
    ASSERT_EQ(width, decoded[row].len_);
    for (int i = 0; i < width; ++i) {
      const char expected = i % 2 == 0 ? row_ids[row] * 64 + i / 2 : 0x80 + i / 2;
      EXPECT_EQ(expected, decoded[row].ptr_[i]);
    }
  }
}

TEST(FtsHotpath, OpaqueIdBlocksSupportAllocationFreeBatchReads)
{
  using namespace oceanbase::blocksstable;
  ObSEArray<oceanbase::share::schema::ObColDesc, 3> columns;
  for (int i = 0; i < 3; ++i) {
    oceanbase::share::schema::ObColDesc column;
    column.col_id_ = 16 + i;
    if (i == 0) {
      column.col_type_.set_varchar();
      column.col_type_.set_collation_type(CS_TYPE_BINARY);
    } else {
      column.col_type_.set_int();
    }
    ASSERT_EQ(OB_SUCCESS, columns.push_back(column));
  }
  ObMicroBlockEncodingCtx encoding;
  encoding.macro_block_size_ = 2 * 1024 * 1024;
  encoding.micro_block_size_ = 16 * 1024;
  encoding.rowkey_column_cnt_ = 3;
  encoding.column_cnt_ = 3;
  encoding.col_descs_ = &columns;
  encoding.compressor_type_ = NONE_COMPRESSOR;
  ObMicroBlockEncoder encoder;
  ASSERT_EQ(OB_SUCCESS, encoder.init(encoding));
  ObDatumRow row;
  ASSERT_EQ(OB_SUCCESS, row.init(3));
  row.row_flag_.set_flag(ObDmlFlag::DF_INSERT);
  row.mvcc_row_flag_.set_last_multi_version_row(true);
  row.storage_datums_[1].set_int(-1);
  row.storage_datums_[2].set_int(0);
  for (int i = 0; i < 128; ++i) {
    char id[16] = {};
    id[0] = 42;
    id[15] = i;
    row.storage_datums_[0].set_string(id, sizeof(id));
    ASSERT_EQ(OB_SUCCESS, encoder.append_row(row));
  }
  char *buffer = nullptr;
  int64_t size = 0;
  ASSERT_EQ(OB_SUCCESS, encoder.build_block(buffer, size));
  ASSERT_GT(size, 0);
  const auto *micro = reinterpret_cast<const ObMicroBlockHeader *>(buffer);
  const auto *headers = reinterpret_cast<const ObColumnHeader *>(buffer + micro->header_size_);
  // Opaque fixed-width IDs should be viewable in-place, without per-row reconstruction.
  ASSERT_EQ(ObColumnHeader::RAW, headers[0].type_);
  const char *meta = reinterpret_cast<const char *>(headers + 3);
  ObRawDecoder decoder;
  ASSERT_EQ(OB_SUCCESS, decoder.init(*micro, headers[0], meta));
  ObWrapperAllocator no_allocations;
  ObColumnDecoderCtx context;
  context.fill(columns[0].col_type_, micro, &headers[0], &no_allocations, 0);
  const int32_t ids[] = {127, 0, 64};
  ObDatum decoded[3];
  const char *cells[3] = {};
  ASSERT_EQ(OB_SUCCESS, decoder.batch_decode(context, nullptr, ids, cells, 3, decoded));
  for (int i = 0; i < 3; ++i) {
    char expected[16] = {};
    expected[0] = 42;
    expected[15] = ids[i];
    ASSERT_EQ(sizeof(expected), decoded[i].len_);
    EXPECT_EQ(0, memcmp(expected, decoded[i].ptr_, sizeof(expected)));
  }
}

TEST(FtsHotpath, DefaultParallelFlagSurvivesIndexTaskTransport)
{
  oceanbase::obcall::ObCreateIndexArg source;
  EXPECT_FALSE(source.is_default_parallel_);
  source.is_default_parallel_ = true;
  source.parallelism_ = 1;
  oceanbase::obcall::ObCreateIndexArg copied;
  ASSERT_EQ(OB_SUCCESS, copied.assign(source));
  EXPECT_TRUE(copied.is_default_parallel_);
  const int64_t size = source.get_serialize_size();
  std::vector<char> buffer(size);
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize(buffer.data(), size, pos));
  ASSERT_EQ(size, pos);
  oceanbase::obcall::ObCreateIndexArg decoded;
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(buffer.data(), size, pos));
  EXPECT_TRUE(decoded.is_default_parallel_);
  EXPECT_EQ(1, decoded.parallelism_);
  decoded.reset();
  EXPECT_FALSE(decoded.is_default_parallel_);
}

int main(int argc, char **argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
