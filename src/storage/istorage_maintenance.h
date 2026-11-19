// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_STORAGE_ISTORAGE_MAINTENANCE_H_
#define SOEASY_SRC_STORAGE_ISTORAGE_MAINTENANCE_H_

#include <cstdint>
#include <string>

#include "common/resource_profile.h"

namespace soeasy {
namespace storage {

struct StorageFragmentation {
  int page_size = 4096;
  int64_t total_pages = 0;
  int64_t free_pages = 0;
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  double frag_ratio = 0.0;
};

/**
 * @brief Narrow role interface for database maintenance, vacuuming, and compaction (ISP).
 */
class IStorageMaintenance {
 public:
  virtual ~IStorageMaintenance() = default;

  virtual bool OptimizeFtsIndex() = 0;
  virtual bool SetBulkMode(bool enable) = 0;
  virtual void SetResourceProfile(common::ResourceProfile profile) = 0;
  virtual bool CheckpointWal() = 0;
  virtual bool GetFragmentation(StorageFragmentation *out_frag) = 0;
  virtual bool IncrementalVacuum(int pages = 500) = 0;
  virtual bool Vacuum() = 0;
};

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_ISTORAGE_MAINTENANCE_H_
