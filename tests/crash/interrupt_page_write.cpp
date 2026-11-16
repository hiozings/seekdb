// Test-only short-write injection: stop after writing half of an 8 KiB page.
// The parent then sends SIGKILL, without modifying database files itself.
#include <dlfcn.h>
#include <unistd.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" ssize_t pwrite(int fd, const void* bytes, size_t size, off_t offset) {
  using Write = ssize_t (*)(int, const void*, size_t, off_t);
  static const auto real_write = reinterpret_cast<Write>(::dlsym(RTLD_NEXT, "pwrite"));
  const char* target = std::getenv("MVDB_TEST_INTERRUPT_FILE");
  char link[64], path[1024];
  std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  const ssize_t len = ::readlink(link, path, sizeof(path)-1);
  if (len >= 0) path[len] = '\0';
  if (target && len >= 0 && size == 8192 && std::strstr(path, target) &&
      (std::strcmp(target, ".btree") != 0 || offset != 0)) {
    static unsigned long matching_writes = 0;
    const char* nth_text = std::getenv("MVDB_TEST_INTERRUPT_NTH");
    const unsigned long nth = nth_text ? std::strtoul(nth_text, nullptr, 10) : 1;
    if (++matching_writes != nth) return real_write(fd, bytes, size, offset);
    const ssize_t written = real_write(fd, bytes, size/2, offset);
    constexpr char marker[] = "HALF_PAGE_WRITTEN\n";
    ::write(STDERR_FILENO, marker, sizeof(marker)-1);
    ::raise(SIGSTOP);
    return written;
  }
  return real_write(fd, bytes, size, offset);
}
