#include "storage/buffer_pool.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using mvdb::BufferPool;
using mvdb::Page;

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class DataFile {
 public:
  DataFile() {
    char pattern[] = "/tmp/mvdb-spill-test-XXXXXX";
    const char* result = ::mkdtemp(pattern);
    require(result != nullptr, "create test directory");
    directory = result;
    path = directory + "/data";
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    require(fd >= 0, "create data file");
  }
  ~DataFile() {
    if (fd >= 0) ::close(fd);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  DataFile(const DataFile&) = delete;
  DataFile& operator=(const DataFile&) = delete;

  std::string directory;
  std::string path;
  int fd = -1;
};

void seed(DataFile& file, uint32_t count) {
  for (uint32_t id = 0; id < count; ++id) {
    Page page;
    page.raw()[100] = 'o';
    require(BufferPool::write_page(file.fd, id, page), "seed original data");
  }
}

void check_original(DataFile& file, uint32_t count) {
  require(::lseek(file.fd, 0, SEEK_END) == static_cast<off_t>(count * Page::kPageSize),
          "uncommitted pages extended data file");
  for (uint32_t id = 0; id < count; ++id) {
    Page page;
    require(BufferPool::read_page(file.fd, id, page), "read original data");
    require(page.raw()[100] == 'o', "uncommitted bytes reached data file");
  }
}

void modify(BufferPool& pool, uint32_t count) {
  for (uint32_t id = 0; id < count; ++id) {
    auto page = pool.fetch(id);
    page->raw()[100] = static_cast<char>(id + 1);
    pool.mark_dirty(page);
    require(pool.size() <= 4, "uncommitted page cache exceeded capacity");
  }
}

void bounded_spill_streaming_and_publication() {
  DataFile file;
  seed(file, 40);
  BufferPool pool(file.fd, 4, file.path);
  pool.set_current_txn(1);
  modify(pool, 40);
  require(pool.sync(), "sync logged pages");
  check_original(file, 40);
  for (uint32_t id = 0; id < 40; ++id) {
    auto page = pool.fetch(id);
    require(page->raw()[100] == static_cast<char>(id + 1), "read-own-writes lost spilled bytes");
    require(pool.size() <= 4, "reading spill exceeded capacity");
  }
  // Reload, edit, and spill the same page twice: the latest version must win.
  {
    auto page = pool.fetch(0);
    page->raw()[100] = 90;
    pool.mark_dirty(page);
  }
  for (uint32_t id = 1; id < 10; ++id) pool.fetch(id);
  {
    auto page = pool.fetch(0);
    require(page->raw()[100] == 90, "first replacement lost");
    page->raw()[100] = 91;
    pool.mark_dirty(page);
  }
  for (uint32_t id = 1; id < 10; ++id) pool.fetch(id);
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<bool> seen(40, false);
    pool.visit_dirty(1, [&](uint32_t id, Page& page) {
      require(id < 40 && !seen[id], "stream duplicated or invented a page");
      seen[id] = true;
      require(page.raw()[100] == (id == 0 ? 91 : static_cast<char>(id + 1)),
              "stream returned stale image");
      if (pass == 0) page.set_lsn(1000 + id);
      else require(page.lsn() == 1000 + id, "streamed LSN was not retained");
      require(pool.size() <= 4, "stream grew cache");
    });
    for (bool present : seen) require(present, "stream missed dirty page");
  }
  check_original(file, 40);
  // The caller would durably log streamed images before release_txn.
  pool.release_txn(1);
  require(pool.sync(), "sync released pages");
  pool.clear();
  for (uint32_t id = 0; id < 40; ++id) {
    auto page = pool.fetch(id);
    require(page->raw()[100] == (id == 0 ? 91 : static_cast<char>(id + 1)),
            "released bytes not preserved");
    require(page->lsn() == 1000 + id, "published image has stale LSN");
  }
}

uint64_t process_written_bytes() {
  std::ifstream input("/proc/self/io");
  std::string name;
  uint64_t value = 0;
  while (input >> name >> value) if (name == "wchar:") return value;
  throw std::runtime_error("cannot read process write counter");
}

void lsn_updates_do_not_rewrite_spilled_payload() {
  DataFile file;
  seed(file, 40);
  BufferPool pool(file.fd, 4, file.path);
  pool.set_current_txn(6);
  modify(pool, 40);
  const uint64_t before = process_written_bytes();
  uint32_t next_spilled = 0;
  pool.visit_dirty(6, [&](uint32_t id, Page& page) {
    if (id < 36) require(id == next_spilled++, "spilled pages were not streamed sequentially");
    page.set_lsn(5000 + id);
  });
  require(next_spilled == 36, "sequential stream missed spilled pages");
  require(process_written_bytes() == before, "LSN-only logging rewrote spilled page payloads");
  {
    auto reloaded = pool.fetch(0);
    require(reloaded->lsn() == 5000, "reloaded spill lost LSN-only change");
  }
  pool.visit_dirty(6, [](uint32_t id, Page& page) {
    require(page.lsn() == 5000 + id, "second visit lost LSN-only change");
    page.raw()[6] = 0x55;
    page.raw()[101] = 'Z';
    page.set_lsn(9000 + id);
  });
  check_original(file, 40);
  pool.release_txn(6);
  require(pool.sync(), "sync after mixed LSN/payload changes");
  pool.clear();
  for (uint32_t id = 0; id < 40; ++id) {
    auto page = pool.fetch(id);
    require(page->lsn() == 9000 + id && page->raw()[101] == 'Z' && page->raw()[6] == 0x55,
            "mixed visitor changes were not published");
  }
}

void pinned_pages_and_compatibility_handles() {
  DataFile file;
  BufferPool pool(file.fd, 4, file.path);
  pool.set_current_txn(2);
  std::vector<std::shared_ptr<Page>> held;
  for (uint32_t id = 0; id < 8; ++id) {
    held.push_back(pool.fetch(id));
    held.back()->raw()[100] = static_cast<char>(id + 1);
    pool.mark_dirty(held.back());
  }
  require(pool.fetch(0).get() == held[0].get(), "pinned page was duplicated");
  held.clear();
  pool.fetch(8);
  require(pool.size() <= 4, "released pins left cache over capacity");
  auto dirty = pool.collect_dirty(2);
  require(dirty.size() == 8, "compatibility collection missed spilled images");
  for (auto& entry : dirty) entry.page->set_lsn(77);
  pool.release_txn(2);
  require(pool.sync(), "sync compatibility changes");
  dirty.clear();
  pool.clear();
  for (uint32_t id = 0; id < 8; ++id) {
    auto page = pool.fetch(id);
    require(page->raw()[100] == static_cast<char>(id + 1) && page->lsn() == 77,
            "compatibility handle changes were lost");
  }
}

void io_failures_preserve_pending_state() {
  DataFile file;
  seed(file, 12);
  ::close(file.fd);
  file.fd = ::open(file.path.c_str(), O_RDONLY);
  require(file.fd >= 0, "open read-only data");
  BufferPool pool(file.fd, 4, file.path);
  pool.set_current_txn(3);
  modify(pool, 12);
  bool failed = false;
  try {
    pool.release_txn(3);
  } catch (const std::system_error&) {
    failed = true;
  }
  require(failed, "failed spill publication was silently accepted");
  size_t count = 0;
  pool.visit_dirty(3, [&](uint32_t id, Page& page) {
    ++count;
    require(page.raw()[100] == static_cast<char>(id + 1), "failed release discarded pending image");
  });
  require(count == 12, "failed release lost ownership");
  check_original(file, 12);

  BufferPool invalid(-1, 4, file.path);
  failed = false;
  try { invalid.fetch(0); } catch (const std::system_error&) { failed = true; }
  require(failed, "read failure was silently accepted");

  BufferPool missing_directory(file.fd, 4, file.directory + "/missing/data");
  missing_directory.set_current_txn(4);
  failed = false;
  try { modify(missing_directory, 5); } catch (const std::system_error&) { failed = true; }
  require(failed, "spill creation failure was silently accepted");
  require(missing_directory.fetch(0)->raw()[100] == 1, "failed spill evicted original frame");
}

void killed_transaction_never_publishes() {
  DataFile file;
  seed(file, 12);
  int ready[2];
  require(::pipe(ready) == 0, "create child handshake");
  const pid_t child = ::fork();
  require(child >= 0, "fork spill writer");
  if (child == 0) {
    ::close(ready[0]);
    try {
      BufferPool pool(file.fd, 4, file.path);
      pool.set_current_txn(5);
      modify(pool, 40);
      require(pool.sync(), "sync skips uncommitted bytes");
      const char signal = 'R';
      if (::write(ready[1], &signal, 1) != 1) ::_exit(2);
      for (;;) ::pause();
    } catch (...) { ::_exit(3); }
  }
  ::close(ready[1]);
  char signal = 0;
  const ssize_t received = ::read(ready[0], &signal, 1);
  ::close(ready[0]);
  ::kill(child, SIGKILL);
  int status = 0;
  require(::waitpid(child, &status, 0) == child, "reap killed child");
  require(received == 1 && signal == 'R' && WIFSIGNALED(status), "child did not reach spill state");
  check_original(file, 12);
  size_t entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(file.directory)) {
    require(entry.path().filename() == "data", "kill leaked spill file");
    ++entries;
  }
  require(entries == 1, "unexpected files after kill");
}

}  // namespace

int main() {
  try {
    lsn_updates_do_not_rewrite_spilled_payload();
    bounded_spill_streaming_and_publication();
    pinned_pages_and_compatibility_handles();
    io_failures_preserve_pending_state();
    killed_transaction_never_publishes();
    std::cout << "buffer pool spill tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
