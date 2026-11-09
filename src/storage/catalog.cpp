#include "storage/catalog.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace mvdb {

namespace {

constexpr char kMagic[8] = {'M', 'V', 'D', 'B', '0', '0', '0', '1'};
constexpr uint32_t kVersion = 2;

void put_u8(std::string& out, uint8_t value) { out.push_back(static_cast<char>(value)); }

void put_u32(std::string& out, uint32_t value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void put_i64(std::string& out, int64_t value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void put_str(std::string& out, const std::string& value) {
  put_u32(out, static_cast<uint32_t>(value.size()));
  out.append(value);
}

class Reader {
 public:
  explicit Reader(std::string_view data) : data_(data) {}

  bool u8(uint8_t& out) {
    if (pos_ + 1 > data_.size()) return false;
    out = static_cast<uint8_t>(data_[pos_++]);
    return true;
  }
  bool u32(uint32_t& out) {
    if (pos_ + sizeof(uint32_t) > data_.size()) return false;
    std::memcpy(&out, data_.data() + pos_, sizeof(out));
    pos_ += sizeof(uint32_t);
    return true;
  }
  bool i64(int64_t& out) {
    if (pos_ + sizeof(int64_t) > data_.size()) return false;
    std::memcpy(&out, data_.data() + pos_, sizeof(out));
    pos_ += sizeof(int64_t);
    return true;
  }
  bool str(std::string& out) {
    uint32_t len = 0;
    if (!u32(len)) return false;
    if (pos_ + len > data_.size()) return false;
    out.assign(data_.data() + pos_, len);
    pos_ += len;
    return true;
  }
  bool done() const { return pos_ == data_.size(); }

 private:
  std::string_view data_;
  size_t pos_ = 0;
};

bool write_all(int fd, const std::string& data) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

bool save_catalog(const std::string& path, const std::vector<TableMeta>& tables,
                  uint32_t next_table_id) {
  std::string buffer(kMagic, sizeof(kMagic));
  put_u32(buffer, kVersion);
  put_u32(buffer, next_table_id);
  put_u32(buffer, static_cast<uint32_t>(tables.size()));
  for (const TableMeta& table : tables) {
    put_u32(buffer, table.id);
    put_str(buffer, table.name);
    put_u32(buffer, static_cast<uint32_t>(table.columns.size()));
    for (const Column& col : table.columns) {
      put_str(buffer, col.name);
      put_u8(buffer, static_cast<uint8_t>(col.type));
      put_i64(buffer, col.length);
      put_i64(buffer, col.dim);
    }
    put_u32(buffer, static_cast<uint32_t>(table.pk_index));
    put_u8(buffer, table.has_vector_index ? 1 : 0);
    put_i64(buffer, table.vector_column);
  }

  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const bool ok = write_all(fd, buffer) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok) {
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  const size_t slash = path.find_last_of('/');
  const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
  const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (dir_fd >= 0) {
    ::fsync(dir_fd);
    ::close(dir_fd);
  }
  return true;
}

bool load_catalog(const std::string& path, std::vector<TableMeta>& tables,
                  uint32_t& next_table_id) {
  tables.clear();
  next_table_id = 1;
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return errno == ENOENT;  // a missing catalog means a fresh data directory
  }
  std::string data;
  char chunk[4096];
  while (true) {
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return false;
    }
    if (n == 0) break;
    data.append(chunk, static_cast<size_t>(n));
  }
  ::close(fd);

  if (data.size() < sizeof(kMagic) || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) {
    return false;
  }
  Reader reader(std::string_view(data).substr(sizeof(kMagic)));
  uint32_t version = 0;
  uint32_t table_count = 0;
  if (!reader.u32(version) || version == 0 || version > kVersion) return false;
  if (!reader.u32(next_table_id)) return false;
  if (!reader.u32(table_count)) return false;
  for (uint32_t i = 0; i < table_count; ++i) {
    TableMeta table;
    uint32_t column_count = 0;
    uint32_t pk_index = 0;
    if (!reader.u32(table.id)) return false;
    if (!reader.str(table.name)) return false;
    if (!reader.u32(column_count)) return false;
    for (uint32_t c = 0; c < column_count; ++c) {
      Column col;
      uint8_t type = 0;
      if (!reader.str(col.name)) return false;
      if (!reader.u8(type)) return false;
      if (!reader.i64(col.length)) return false;
      if (!reader.i64(col.dim)) return false;
      if (type > 2) return false;
      col.type = static_cast<ColumnType>(type);
      table.columns.push_back(std::move(col));
    }
    if (!reader.u32(pk_index)) return false;
    table.pk_index = pk_index;
    if (table.pk_index >= table.columns.size()) return false;
    if (version >= 2) {
      uint8_t has_index = 0;
      int64_t vector_column = -1;
      if (!reader.u8(has_index)) return false;
      if (!reader.i64(vector_column)) return false;
      table.has_vector_index = has_index != 0;
      table.vector_column = static_cast<int32_t>(vector_column);
      if (table.has_vector_index &&
          (table.vector_column < 0 || static_cast<size_t>(table.vector_column) >= table.columns.size())) {
        return false;
      }
    }
    tables.push_back(std::move(table));
  }
  return reader.done();
}

}  // namespace mvdb
