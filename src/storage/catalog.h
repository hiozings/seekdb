#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "storage/schema.h"

namespace mvdb {

// The catalog is the authoritative description of table schemas. It is
// rewritten atomically (temp file + fsync + rename) so a crash leaves either
// the old or the new catalog, never a torn one.
std::string encode_catalog(const std::vector<TableMeta>& tables, uint32_t next_table_id);
bool decode_catalog(std::string_view data, std::vector<TableMeta>& tables, uint32_t& next_table_id);
bool load_catalog(const std::string& path, std::vector<TableMeta>& tables,
                  uint32_t& next_table_id);
bool save_catalog(const std::string& path, const std::vector<TableMeta>& tables,
                  uint32_t next_table_id);

}  // namespace mvdb
