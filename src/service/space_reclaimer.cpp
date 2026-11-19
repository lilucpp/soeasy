// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "service/space_reclaimer.h"

#include <algorithm>

#include "common/logger.h"

namespace soeasy {
namespace service {

SpaceReclaimer::SpaceReclaimer() = default;

SpaceReclaimer::~SpaceReclaimer() {
  StopAndJoin();
}

void SpaceReclaimer::StopAndJoin() {
  stop_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lock(mutex_);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void SpaceReclaimer::TriggerAsyncReclaim(std::shared_ptr<storage::IStorageMaintenance> storage_engine) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (is_reclaiming_.load(std::memory_order_acquire)) {
    LOG_DEBUG("[SpaceReclaimer] Space reclaim already in progress, skipping.");
    return;
  }
  if (thread_.joinable()) {
    thread_.join();
  }

  stop_.store(false, std::memory_order_release);
  is_reclaiming_.store(true, std::memory_order_release);

  thread_ = std::thread([this, storage = std::move(storage_engine)]() {
    struct ReclaimFlagGuard {
      explicit ReclaimFlagGuard(std::atomic<bool> *flag) : flag_(flag) {}
      ~ReclaimFlagGuard() {
        if (flag_) flag_->store(false, std::memory_order_release);
      }
      ReclaimFlagGuard(const ReclaimFlagGuard &) = delete;
      ReclaimFlagGuard &operator=(const ReclaimFlagGuard &) = delete;
      std::atomic<bool> *flag_;
    } flag_guard(&is_reclaiming_);

    LOG_INFO("[SpaceReclaimer] Starting asynchronous space reclamation check...");
    if (!storage || stop_.load(std::memory_order_acquire)) {
      return;
    }

    storage::StorageFragmentation frag;
    if (!storage->GetFragmentation(&frag)) {
      LOG_WARN("[SpaceReclaimer] Failed to query database fragmentation status.");
      return;
    }

    LOG_INFO("[SpaceReclaimer] Storage status: total={:.2f} MB, free={:.2f} MB ({:.1f}%), free_pages={}",
             frag.total_bytes / (1024.0 * 1024.0), frag.free_bytes / (1024.0 * 1024.0), frag.frag_ratio * 100.0,
             frag.free_pages);

    if (frag.free_pages >= kMinFreePagesForVacuum || frag.frag_ratio >= kMinFragRatioForVacuum) {
      LOG_INFO("[SpaceReclaimer] Fragmentation exceeds threshold. Initiating paced incremental vacuum...");

      int64_t pages_left = frag.free_pages;
      while (pages_left > 0 && !stop_.load(std::memory_order_acquire)) {
        int pages_to_clean = static_cast<int>(std::min<int64_t>(pages_left, kVacuumChunkPages));
        if (!storage->IncrementalVacuum(pages_to_clean)) {
          LOG_WARN("[SpaceReclaimer] Incremental vacuum failed during paced cleanup.");
          break;
        }
        pages_left -= pages_to_clean;
        std::this_thread::sleep_for(kVacuumPacingInterval);
      }
      LOG_INFO("[SpaceReclaimer] Paced incremental vacuum completed successfully.");
    } else {
      LOG_DEBUG("[SpaceReclaimer] Fragmentation level is healthy, no vacuum required.");
    }
  });
}

}  // namespace service
}  // namespace soeasy
