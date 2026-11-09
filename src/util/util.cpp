#include "util/util.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace mvdb {

std::string to_lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

std::string trim(std::string_view s) {
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) {
    ++begin;
  }
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
    --end;
  }
  return std::string(s.substr(begin, end - begin));
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool is_valid_utf8(std::string_view s) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t extra = 0;
    unsigned int cp = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      extra = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + extra >= n) return false;
    for (size_t k = 1; k <= extra; ++k) {
      unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (extra == 1 && cp < 0x80) return false;
    if (extra == 2 && cp < 0x800) return false;
    if (extra == 3 && cp < 0x10000) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    i += extra + 1;
  }
  return true;
}

size_t utf8_codepoint_count(std::string_view s) {
  size_t count = 0;
  for (char ch : s) {
    unsigned char c = static_cast<unsigned char>(ch);
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

bool parse_int64(std::string_view s, int64_t& out) {
  std::string t = trim(s);
  if (t.empty()) return false;
  const char* first = t.data();
  const char* last = t.data() + t.size();
  if (*first == '+') ++first;
  if (first == last) return false;
  int64_t value = 0;
  auto res = std::from_chars(first, last, value, 10);
  if (res.ec != std::errc() || res.ptr != last) return false;
  out = value;
  return true;
}

bool parse_double(std::string_view s, double& out) {
  std::string t = trim(s);
  if (t.empty()) return false;
  const char* first = t.data();
  const char* last = t.data() + t.size();
  if (*first == '+') ++first;
  if (first == last) return false;
  double value = 0.0;
  auto res = std::from_chars(first, last, value, std::chars_format::general);
  if (res.ec != std::errc() || res.ptr != last) return false;
  out = value;
  return true;
}

std::string format_double(double v, std::string_view fmt) {
  std::string f(fmt);
  char buf[512];
  int n = std::snprintf(buf, sizeof(buf), f.c_str(), v);
  if (n < 0) return std::string();
  if (static_cast<size_t>(n) >= sizeof(buf)) n = static_cast<int>(sizeof(buf) - 1);
  return std::string(buf, static_cast<size_t>(n));
}

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == sep) {
      out.emplace_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

}  // namespace mvdb
