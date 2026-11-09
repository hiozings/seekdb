#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "storage/schema.h"

namespace mvdb {

// Row encoding is schema-driven: no per-value type tags are stored.
//   BIGINT  -> 8 bytes, little endian
//   VARCHAR -> uint32 byte length + UTF-8 bytes
//   VECTOR  -> dim * 4 bytes of IEEE-754 float32
std::string encode_row(const TableMeta& meta, const std::vector<Value>& values);

// Returns false when the byte range does not decode against the schema.
bool decode_row(const TableMeta& meta, std::string_view bytes, std::vector<Value>& out);

// Decodes just the primary key of an encoded row.
bool decode_primary_key(const TableMeta& meta, std::string_view bytes, int64_t& out);

}  // namespace mvdb
