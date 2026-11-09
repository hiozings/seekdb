#include "storage/record.h"

#include <cstring>

namespace mvdb {

namespace {

void put_u32(std::string& out, uint32_t value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void put_i64(std::string& out, int64_t value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool get_u32(std::string_view bytes, size_t& pos, uint32_t& out) {
  if (pos + sizeof(uint32_t) > bytes.size()) return false;
  std::memcpy(&out, bytes.data() + pos, sizeof(out));
  pos += sizeof(uint32_t);
  return true;
}

bool get_i64(std::string_view bytes, size_t& pos, int64_t& out) {
  if (pos + sizeof(int64_t) > bytes.size()) return false;
  std::memcpy(&out, bytes.data() + pos, sizeof(out));
  pos += sizeof(int64_t);
  return true;
}

}  // namespace

std::string encode_row(const TableMeta& meta, const std::vector<Value>& values) {
  std::string out;
  for (size_t i = 0; i < meta.columns.size(); ++i) {
    const Column& col = meta.columns[i];
    const Value& value = values[i];
    switch (col.type) {
      case ColumnType::BigInt:
        put_i64(out, value.i);
        break;
      case ColumnType::Varchar: {
        put_u32(out, static_cast<uint32_t>(value.s.size()));
        out.append(value.s);
        break;
      }
      case ColumnType::Vector: {
        const size_t bytes = value.v.size() * sizeof(float);
        out.append(reinterpret_cast<const char*>(value.v.data()), bytes);
        break;
      }
    }
  }
  return out;
}

bool decode_row(const TableMeta& meta, std::string_view bytes, std::vector<Value>& out) {
  size_t pos = 0;
  out.clear();
  out.reserve(meta.columns.size());
  for (const Column& col : meta.columns) {
    switch (col.type) {
      case ColumnType::BigInt: {
        int64_t value = 0;
        if (!get_i64(bytes, pos, value)) return false;
        out.push_back(Value::make_int(value));
        break;
      }
      case ColumnType::Varchar: {
        uint32_t len = 0;
        if (!get_u32(bytes, pos, len)) return false;
        if (pos + len > bytes.size()) return false;
        out.push_back(Value::make_str(std::string(bytes.substr(pos, len))));
        pos += len;
        break;
      }
      case ColumnType::Vector: {
        const size_t dim = static_cast<size_t>(col.dim);
        const size_t bytes_needed = dim * sizeof(float);
        if (pos + bytes_needed > bytes.size()) return false;
        std::vector<float> vec(dim);
        std::memcpy(vec.data(), bytes.data() + pos, bytes_needed);
        pos += bytes_needed;
        out.push_back(Value::make_vec(std::move(vec)));
        break;
      }
    }
  }
  return pos == bytes.size();
}

bool decode_primary_key(const TableMeta& meta, std::string_view bytes, int64_t& out) {
  size_t pos = 0;
  for (size_t i = 0; i < meta.columns.size(); ++i) {
    const Column& col = meta.columns[i];
    switch (col.type) {
      case ColumnType::BigInt: {
        int64_t value = 0;
        if (!get_i64(bytes, pos, value)) return false;
        if (i == meta.pk_index) {
          out = value;
          return true;
        }
        break;
      }
      case ColumnType::Varchar: {
        uint32_t len = 0;
        if (!get_u32(bytes, pos, len)) return false;
        pos += len;
        if (pos > bytes.size()) return false;
        break;
      }
      case ColumnType::Vector: {
        pos += static_cast<size_t>(col.dim) * sizeof(float);
        if (pos > bytes.size()) return false;
        break;
      }
    }
  }
  return false;
}

}  // namespace mvdb
