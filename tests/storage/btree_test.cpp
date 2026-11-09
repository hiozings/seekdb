// Randomized comparison of the on-disk B+tree against std::map.
//
// Build with -DMVDB_BUILD_TESTS=ON and run build/btree_test.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <unistd.h>

#include "storage/btree.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

std::string make_temp_dir() {
  char tmpl[] = "/tmp/mvdb-btree-XXXXXX";
  const char* dir = ::mkdtemp(tmpl);
  if (dir == nullptr) {
    std::perror("mkdtemp");
    std::exit(2);
  }
  return std::string(dir);
}

}  // namespace

int main() {
  const std::string dir = make_temp_dir();
  const std::string path = dir + "/pk.btree";
  constexpr size_t kPoolPages = 16;  // deliberately small to exercise eviction

  std::map<int64_t, mvdb::RID> reference;
  mvdb::BTree tree(path, kPoolPages);
  if (!tree.open()) {
    std::printf("FAIL: open\n");
    return 1;
  }
  check(!tree.built(), "fresh tree is not built yet");
  check(tree.mark_built(), "mark_built");

  std::mt19937_64 rng(20261109);
  constexpr int kKeyspace = 20000;

  // 1. random inserts
  for (int i = 0; i < 12000; ++i) {
    const int64_t key = static_cast<int64_t>(rng() % kKeyspace);
    const mvdb::RID rid = (static_cast<mvdb::RID>(i) << 16) | 1;
    tree.insert(key, rid);
    reference[key] = rid;
  }
  check(tree.size() == reference.size(), "size after random inserts");

  for (const auto& entry : reference) {
    mvdb::RID rid = 0;
    if (!tree.search(entry.first, rid) || rid != entry.second) {
      check(false, "lookup after random inserts, key " + std::to_string(entry.first));
      break;
    }
  }

  // 2. sequential inserts force rightmost splits
  for (int64_t key = kKeyspace; key < kKeyspace + 5000; ++key) {
    const mvdb::RID rid = static_cast<mvdb::RID>(key);
    tree.insert(key, rid);
    reference[key] = rid;
  }
  check(tree.size() == reference.size(), "size after sequential inserts");

  // 3. upsert keeps the key count and replaces the RID
  {
    const int64_t key = 42;
    const mvdb::RID rid = 0xABCDEF;
    tree.insert(key, rid);
    reference[key] = rid;
    mvdb::RID found = 0;
    check(tree.search(key, found) && found == rid, "upsert replaces the RID");
    check(tree.size() == reference.size(), "size unchanged by upsert");
  }

  // 4. random erases
  for (int i = 0; i < 6000; ++i) {
    const int64_t key = static_cast<int64_t>(rng() % kKeyspace);
    const bool existed = reference.erase(key) > 0;
    const bool erased = tree.erase(key);
    if (existed != erased) {
      check(false, "erase disagreement on key " + std::to_string(key));
      break;
    }
  }
  check(tree.size() == reference.size(), "size after erases");
  check(!tree.erase(-999999), "erasing a missing key fails");

  // 5. reopen and verify everything survived
  const uint64_t before = tree.size();
  tree.close();
  mvdb::BTree reopened(path, kPoolPages);
  if (!reopened.open()) {
    std::printf("FAIL: reopen\n");
    return 1;
  }
  check(reopened.built(), "reopened tree is built");
  check(reopened.size() == before, "size survives reopen");
  for (const auto& entry : reference) {
    mvdb::RID rid = 0;
    if (!reopened.search(entry.first, rid) || rid != entry.second) {
      check(false, "lookup after reopen, key " + std::to_string(entry.first));
      break;
    }
  }

  // 6. every erased key must be gone
  for (int i = 0; i < 2000; ++i) {
    const int64_t key = static_cast<int64_t>(rng() % kKeyspace);
    if (reference.count(key) != 0) continue;
    mvdb::RID rid = 0;
    if (reopened.search(key, rid)) {
      check(false, "erased key still visible: " + std::to_string(key));
      break;
    }
  }

  reopened.close();
  std::string cleanup = "rm -rf " + dir;
  if (std::system(cleanup.c_str()) != 0) {
    std::printf("warning: could not remove %s\n", dir.c_str());
  }

  if (failures == 0) {
    std::printf("btree_test: all checks passed\n");
    return 0;
  }
  std::printf("btree_test: %d failure(s)\n", failures);
  return 1;
}
