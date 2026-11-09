#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mvdb {

std::string to_lower(std::string_view s);
std::string trim(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool istarts_with(std::string_view s, std::string_view prefix);

// Number of Unicode code points in a valid UTF-8 byte sequence.
size_t utf8_codepoint_count(std::string_view s);
bool is_valid_utf8(std::string_view s);

bool parse_int64(std::string_view s, int64_t& out);
bool parse_double(std::string_view s, double& out);
// Renders a double using a printf-style format ("%.6f" by default).
std::string format_double(double v, std::string_view fmt);

std::vector<std::string> split(std::string_view s, char sep);

}  // namespace mvdb
