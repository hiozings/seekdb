#pragma once

#include <cstddef>
#include <cstdint>

namespace mvdb {

// CRC-32 (IEEE 802.3 polynomial, reflected), table-free implementation.
inline uint32_t crc32(const void* data, size_t len, uint32_t seed = 0) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = ~seed;
  for (size_t i = 0; i < len; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = (crc & 1U) ? 0xEDB88320U : 0U;
      crc = (crc >> 1) ^ mask;
    }
  }
  return ~crc;
}

}  // namespace mvdb
