// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_COMMON_RESOURCE_PROFILE_H_
#define SOEASY_SRC_COMMON_RESOURCE_PROFILE_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace soeasy {
namespace common {

/**
 * @brief Resource allocation & persistence policy presets.
 *
 * All profiles enable baseline memory hygiene (malloc_trim on idle, SQLite
 * shrink_memory, WAL truncation on ClearAll, ZSTD_CCtx/SQLITE_STATIC reuse,
 * mmap=0 for encrypted databases, and M_ARENA_MAX=2 on glibc).
 */
enum class ResourceProfile {
  kLowMemory = 0,  // Default: fast indexing & search with minimal resident RAM footprint
  kLowWrite = 1,   // Reduced disk write amplification with larger batching & commit interval
};

struct ResourceTuning {
  ResourceProfile profile = ResourceProfile::kLowMemory;

  // AsyncWriteQueue backpressure and batching
  size_t queue_max_items = 150;
  size_t queue_batch_size = 100;
  size_t queue_max_bytes = 16ULL * 1024 * 1024;

  // SQLite writer & WAL settings (negative cache_size = KiB)
  int cache_idle_kb = -8000;
  int cache_bulk_kb = -16000;
  int wal_autocheckpoint_bulk = 3000;
  int wal_autocheckpoint_idle = 1000;
  int64_t mmap_bytes = 0;

  // SQLite reader connection pool settings
  size_t reader_pool_size = 2;
  int reader_cache_kb = -4000;
};

inline constexpr ResourceTuning GetResourceTuning(ResourceProfile profile) {
  if (profile == ResourceProfile::kLowWrite) {
    return ResourceTuning{
        ResourceProfile::kLowWrite,
        300,                  // queue_max_items
        200,                  // queue_batch_size
        32ULL * 1024 * 1024,  // queue_max_bytes (32 MiB)
        -8000,                // cache_idle_kb (8 MiB)
        -32000,               // cache_bulk_kb (32 MiB)
        10000,                // wal_autocheckpoint_bulk
        1000,                 // wal_autocheckpoint_idle
        0,                    // mmap_bytes (disabled for AES-encrypted DB)
        2,                    // reader_pool_size
        -4000                 // reader_cache_kb (4 MiB)
    };
  }
  return ResourceTuning{
      ResourceProfile::kLowMemory,
      150,                  // queue_max_items
      100,                  // queue_batch_size
      16ULL * 1024 * 1024,  // queue_max_bytes (16 MiB)
      -8000,                // cache_idle_kb (8 MiB)
      -16000,               // cache_bulk_kb (16 MiB)
      3000,                 // wal_autocheckpoint_bulk
      1000,                 // wal_autocheckpoint_idle
      0,                    // mmap_bytes (disabled for AES-encrypted DB)
      2,                    // reader_pool_size
      -4000                 // reader_cache_kb (4 MiB)
  };
}

inline std::string ResourceProfileToString(ResourceProfile profile) {
  return (profile == ResourceProfile::kLowWrite) ? "low_write" : "low_memory";
}

inline ResourceProfile ParseResourceProfile(const std::string &value) {
  return (value == "low_write") ? ResourceProfile::kLowWrite : ResourceProfile::kLowMemory;
}

}  // namespace common
}  // namespace soeasy

#endif  // SOEASY_SRC_COMMON_RESOURCE_PROFILE_H_
