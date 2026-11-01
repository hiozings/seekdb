/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdint>

namespace oceanbase { namespace storage {

// Built-in dictionaries/stopwords are immutable within one initialized FTS
// subsystem lifetime. Zero means unavailable. Values are never reused across
// an embedded shutdown/restart, even if the allocator reuses the same address.
uint64_t fts_parser_generation();

} }
