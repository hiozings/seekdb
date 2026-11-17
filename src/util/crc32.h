#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mvdb {
namespace detail {

constexpr auto make_crc32_tables() {
  std::array<std::array<uint32_t, 256>, 8> tables{};
  for (size_t value = 0; value < 256; ++value) {
    uint32_t crc = static_cast<uint32_t>(value);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
    tables[0][value] = crc;
  }
  for (size_t slice = 1; slice < tables.size(); ++slice) {
    for (size_t value = 0; value < 256; ++value) {
      const uint32_t crc = tables[slice - 1][value];
      tables[slice][value] = (crc >> 8) ^ tables[0][crc & 0xFFU];
    }
  }
  return tables;
}

inline constexpr auto kCrc32Tables = make_crc32_tables();

}  // namespace detail

// IEEE CRC-32, including the original incremental seed convention. Processing
// eight bytes at a time keeps large page-image commits out of the bitwise loop.
// Explicit byte assembly supports unaligned data without endian assumptions.
inline uint32_t crc32(const void* data, size_t len, uint32_t seed = 0) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  const auto& table = detail::kCrc32Tables;
  uint32_t crc = ~seed;
  while (len >= 8) {
    const uint32_t first = crc ^ (static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24));
    crc = table[7][first & 0xFFU] ^ table[6][(first >> 8) & 0xFFU] ^
          table[5][(first >> 16) & 0xFFU] ^ table[4][first >> 24] ^
          table[3][bytes[4]] ^ table[2][bytes[5]] ^ table[1][bytes[6]] ^ table[0][bytes[7]];
    bytes += 8;
    len -= 8;
  }
  while (len != 0) {
    crc = (crc >> 8) ^ table[0][(crc ^ *bytes++) & 0xFFU];
    --len;
  }
  return ~crc;
}

}  // namespace mvdb
