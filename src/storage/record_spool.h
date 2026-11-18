#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <cerrno>
#include <string>
#include <string_view>
#include <unistd.h>

namespace mvdb {

// Process-local temporary records. The unlinked file lives beside the data,
// is never recovery input, and disappears automatically on process exit.
class RecordSpool {
 public:
  explicit RecordSpool(std::string directory) : directory_(std::move(directory)) {}
  ~RecordSpool() { if (file_) std::fclose(file_); }
  RecordSpool(const RecordSpool&) = delete;
  RecordSpool& operator=(const RecordSpool&) = delete;

  size_t size() const { return count_; }
  void append(std::string_view record) {
    open();
    // A seek on an output stream flushes stdio. Consecutive appends already
    // have the correct position, including data still in the stdio buffer.
    if (!appending_) seek(end_);
    const uint64_t len = record.size();
    write(&len, sizeof(len));
    write(record.data(), record.size());
    write(&len, sizeof(len));
    end_ += record.size() + 2 * sizeof(len);
    ++count_;
    appending_ = true;
  }
  std::string back() {
    if (count_ == 0) throw std::system_error(errno ? errno : EIO, std::generic_category(), "empty temporary log");
    seek(end_ - sizeof(uint64_t));
    uint64_t len = 0;
    read(&len, sizeof(len));
    seek(end_ - sizeof(uint64_t) - len);
    std::string record(len, '\0');
    read(record.data(), record.size());
    return record;
  }
  void pop_back() {
    seek(end_ - sizeof(uint64_t));
    uint64_t len = 0;
    read(&len, sizeof(len));
    end_ -= len + 2 * sizeof(len);
    --count_;
  }
  void clear() {
    if (file_) { std::fclose(file_); file_ = nullptr; }
    end_ = 0;
    appending_ = false;
    count_ = 0;
  }
  template <typename Fn> void for_each(Fn fn) {
    if (!file_) return;
    seek(0);
    for (size_t i = 0; i < count_; ++i) {
      uint64_t len = 0, footer = 0;
      read(&len, sizeof(len));
      std::string record(len, '\0');
      read(record.data(), record.size());
      read(&footer, sizeof(footer));
      if (footer != len) throw std::system_error(errno ? errno : EIO, std::generic_category(), "invalid temporary record");
      fn(std::string_view(record));
    }
  }
 private:
  void open() {
    if (file_) return;
    std::string name = directory_ + "/mvdb-spool-XXXXXX";
    const int fd = ::mkstemp(name.data());
    if (fd < 0) throw std::system_error(errno ? errno : EIO, std::generic_category(), "cannot create temporary record file");
    ::unlink(name.c_str());
    file_ = ::fdopen(fd, "w+b");
    if (!file_) { ::close(fd); throw std::system_error(errno ? errno : EIO, std::generic_category(), "cannot open temporary record file"); }
  }
  void seek(uint64_t pos) {
    appending_ = false;
    if (::fseeko(file_, static_cast<off_t>(pos), SEEK_SET) != 0)
      throw std::system_error(errno ? errno : EIO, std::generic_category(), "cannot seek temporary record file");
  }
  void read(void* data, size_t size) {
    if (size && std::fread(data, 1, size, file_) != size)
      throw std::system_error(errno ? errno : EIO, std::generic_category(), "cannot read temporary record file");
  }
  void write(const void* data, size_t size) {
    if (size && std::fwrite(data, 1, size, file_) != size)
      throw std::system_error(errno ? errno : EIO, std::generic_category(), "cannot write temporary record file");
  }
  std::string directory_;
  FILE* file_ = nullptr;
  uint64_t end_ = 0;
  bool appending_ = false;
  size_t count_ = 0;
};

}  // namespace mvdb
