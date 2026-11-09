#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace mvdb {

// Write-ahead log of full page images.
//
// Durability model: a transaction's modified pages are logged at commit time,
// followed by a COMMIT record; the log is fsynced before the commit response is
// produced. Modified pages are never written to a data file while their owning
// transaction is active (the buffer pool refuses to evict them), so the only
// way uncommitted bytes can reach a data file is through a bug. Recovery
// therefore only has to replay committed images; there is nothing to undo.
//
// Images carry a page LSN that is monotonic across restarts (epoch << 48 |
// counter), so replaying a log twice, or replaying after a crash during
// recovery, converges to the same state.
class Wal {
 public:
  enum class Type : uint8_t {
    kPageImage = 1,
    kCommit = 2,
    kCheckpoint = 3,
  };

  struct Record {
    Type type = Type::kPageImage;
    uint64_t lsn = 0;
    uint64_t txid = 0;
    uint32_t file_tag = 0;
    uint32_t page_id = 0;
    const char* payload = nullptr;
    size_t payload_len = 0;
  };

  Wal() = default;
  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  bool open(const std::string& log_path, std::string& error);
  void close();

  uint64_t allocate_lsn() { return (epoch_ << 48) | (++counter_); }

  bool log_page_image(uint64_t txid, uint32_t file_tag, uint32_t page_id, uint64_t lsn,
                      const char* image, size_t image_len);
  bool log_commit(uint64_t txid);
  bool log_checkpoint(uint64_t lsn);

  // Flushes buffered records and fdatasyncs the log.
  bool sync();

  // Streams every record in order. Returns false only on an I/O error; a torn
  // or corrupt tail ends the stream normally.
  bool scan(const std::function<bool(const Record&)>& fn);

  // Empties the log. Only safe after every data page has been fsynced.
  bool truncate();

  const std::string& path() const { return path_; }

 private:
  bool flush_buffer();
  bool write_epoch(uint64_t epoch);

  std::string path_;
  std::string epoch_path_;
  int fd_ = -1;
  uint64_t epoch_ = 0;
  uint64_t counter_ = 0;
  std::string buffer_;
};

}  // namespace mvdb
