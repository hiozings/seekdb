#include "txn/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "util/crc32.h"

namespace mvdb {

namespace {

constexpr uint32_t kRecordMagic = 0x4D564457;  // "MVDW"
constexpr size_t kHeaderSize = 40;
constexpr size_t kMaxPayload = 64 * 1024;
constexpr size_t kFlushThreshold = 1024 * 1024;
constexpr char kEpochMagic[8] = {'M', 'V', 'D', 'B', 'E', 'P', 'O', '1'};

// Record layout (40-byte header):
//   0  u32 magic
//   4  u8  type
//   5  u8  flags
//   6  u16 reserved
//   8  u64 lsn
//  16  u64 txid
//  24  u32 file_tag
//  28  u32 page_id
//  32  u32 payload_len
//  36  u32 crc          over bytes [0, 36) followed by the payload

void put_u32(std::string& out, uint32_t value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}
void put_u64(std::string& out, uint64_t value) {
  out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

uint32_t get_u32(const char* p) {
  uint32_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}
uint64_t get_u64(const char* p) {
  uint64_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

bool write_all(int fd, const char* data, size_t len) {
  size_t written = 0;
  while (written < len) {
    const ssize_t n = ::write(fd, data + written, len - written);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

std::string build_record(Wal::Type type, uint64_t lsn, uint64_t txid, uint32_t file_tag,
                         uint32_t page_id, const char* payload, size_t payload_len) {
  std::string record;
  record.reserve(kHeaderSize + payload_len);
  put_u32(record, kRecordMagic);
  record.push_back(static_cast<char>(type));
  record.push_back(0);
  record.push_back(0);
  record.push_back(0);
  put_u64(record, lsn);
  put_u64(record, txid);
  put_u32(record, file_tag);
  put_u32(record, page_id);
  put_u32(record, static_cast<uint32_t>(payload_len));
  put_u32(record, 0);  // CRC placeholder, patched below
  if (payload_len != 0) record.append(payload, payload_len);
  uint32_t crc = crc32(record.data(), kHeaderSize - 4);
  if (payload_len != 0) crc = crc32(record.data() + kHeaderSize, payload_len, crc);
  std::memcpy(record.data() + kHeaderSize - 4, &crc, sizeof(crc));
  return record;
}

}  // namespace

Wal::~Wal() { close(); }

bool Wal::write_epoch(uint64_t epoch) {
  const std::string tmp = epoch_path_ + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  std::string payload(kEpochMagic, sizeof(kEpochMagic));
  put_u64(payload, epoch);
  const bool ok = write_all(fd, payload.data(), payload.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok) return false;
  return ::rename(tmp.c_str(), epoch_path_.c_str()) == 0;
}

bool Wal::open(const std::string& log_path, std::string& error) {
  path_ = log_path;
  epoch_path_ = log_path + ".epoch";
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) {
    error = "cannot open write-ahead log '" + path_ + "': " + std::strerror(errno);
    return false;
  }
  ::lseek(fd_, 0, SEEK_END);

  // The epoch keeps page LSNs monotonic across restarts. Without it a fresh log
  // would hand out LSNs older than those already stored in data pages, and
  // replay would skip images that still need to be applied.
  uint64_t epoch = 0;
  const int efd = ::open(epoch_path_.c_str(), O_RDONLY);
  if (efd >= 0) {
    char buf[16];
    const ssize_t n = ::read(efd, buf, sizeof(buf));
    ::close(efd);
    if (n == static_cast<ssize_t>(sizeof(kEpochMagic) + 8) &&
        std::memcmp(buf, kEpochMagic, sizeof(kEpochMagic)) == 0) {
      epoch = get_u64(buf + sizeof(kEpochMagic));
    }
  }
  epoch_ = epoch + 1;
  counter_ = 0;
  if (!write_epoch(epoch_)) {
    error = "cannot persist write-ahead log epoch";
    return false;
  }
  buffer_.clear();
  return true;
}

void Wal::close() {
  if (fd_ >= 0) {
    sync();
    ::close(fd_);
    fd_ = -1;
  }
  buffer_.clear();
}

bool Wal::flush_buffer() {
  if (buffer_.empty()) return true;
  if (!write_all(fd_, buffer_.data(), buffer_.size())) return false;
  buffer_.clear();
  return true;
}

bool Wal::sync() {
  if (fd_ < 0) return false;
  if (!flush_buffer()) return false;
  return ::fdatasync(fd_) == 0;
}

bool Wal::log_page_image(uint64_t txid, uint32_t file_tag, uint32_t page_id, uint64_t lsn,
                         const char* image, size_t image_len) {
  if (fd_ < 0 || image_len == 0 || image_len > kMaxPayload) return false;
  buffer_ += build_record(Type::kPageImage, lsn, txid, file_tag, page_id, image, image_len);
  if (buffer_.size() >= kFlushThreshold) return flush_buffer();
  return true;
}

bool Wal::log_commit(uint64_t txid) {
  if (fd_ < 0) return false;
  buffer_ += build_record(Type::kCommit, allocate_lsn(), txid, 0, 0, nullptr, 0);
  if (buffer_.size() >= kFlushThreshold) return flush_buffer();
  return true;
}

bool Wal::log_checkpoint(uint64_t lsn) {
  if (fd_ < 0) return false;
  buffer_ += build_record(Type::kCheckpoint, lsn, 0, 0, 0, nullptr, 0);
  return true;
}

bool Wal::truncate() {
  if (fd_ < 0) return false;
  buffer_.clear();
  if (::ftruncate(fd_, 0) != 0) return false;
  ::lseek(fd_, 0, SEEK_SET);
  return ::fsync(fd_) == 0;
}

bool Wal::scan(const std::function<bool(const Record&)>& fn) {
  if (fd_ < 0) return false;
  if (!flush_buffer()) return false;
  const int rfd = ::open(path_.c_str(), O_RDONLY);
  if (rfd < 0) return false;
  ::lseek(rfd, 0, SEEK_SET);
  std::string pending;
  char chunk[64 * 1024];
  bool eof = false;
  bool stopped = false;
  while (!stopped) {
    while (pending.size() >= kHeaderSize) {
      const char* p = pending.data();
      if (get_u32(p) != kRecordMagic) {
        stopped = true;
        break;
      }
      const uint32_t payload_len = get_u32(p + 32);
      if (payload_len > kMaxPayload) {
        stopped = true;
        break;
      }
      const size_t total = kHeaderSize + payload_len;
      if (pending.size() < total) break;
      const uint32_t stored_crc = get_u32(p + 36);
      uint32_t actual_crc = crc32(p, kHeaderSize - 4);
      if (payload_len != 0) actual_crc = crc32(p + kHeaderSize, payload_len, actual_crc);
      if (stored_crc != actual_crc) {
        stopped = true;
        break;
      }
      Record record;
      record.type = static_cast<Type>(static_cast<uint8_t>(p[4]));
      record.lsn = get_u64(p + 8);
      record.txid = get_u64(p + 16);
      record.file_tag = get_u32(p + 24);
      record.page_id = get_u32(p + 28);
      record.payload = p + kHeaderSize;
      record.payload_len = payload_len;
      const bool keep_going = fn(record);
      pending.erase(0, total);
      if (!keep_going) {
        stopped = true;
        break;
      }
    }
    if (stopped) break;
    if (eof) break;
    const ssize_t n = ::read(rfd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(rfd);
      return false;
    }
    if (n == 0) {
      eof = true;
      continue;
    }
    pending.append(chunk, static_cast<size_t>(n));
  }
  ::close(rfd);
  return true;
}

}  // namespace mvdb
