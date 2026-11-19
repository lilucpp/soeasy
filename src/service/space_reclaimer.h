// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_SPACE_RECLAIMER_H_
#define SOEASY_SRC_SERVICE_SPACE_RECLAIMER_H_

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "storage/istorage_maintenance.h"

namespace soeasy {
namespace service {

/**
 * @brief Asynchronous SQLite space reclamation and incremental vacuum pacing engine.
 *
 * Runs low-priority maintenance in a background thread with pacing to avoid blocking live searches.
 * Depends strictly on the narrow IStorageMaintenance interface (ISP).
 */
class SpaceReclaimer {
 public:
  static constexpr int64_t kMinFreePagesForVacuum = 100;
  static constexpr double kMinFragRatioForVacuum = 0.15;
  static constexpr int kVacuumChunkPages = 250;
  static constexpr std::chrono::milliseconds kVacuumPacingInterval{20};

  SpaceReclaimer();
  ~SpaceReclaimer();

  // Non-copyable, non-movable
  SpaceReclaimer(const SpaceReclaimer &) = delete;
  SpaceReclaimer &operator=(const SpaceReclaimer &) = delete;

  void TriggerAsyncReclaim(std::shared_ptr<storage::IStorageMaintenance> storage_engine);
  void StopAndJoin();
  [[nodiscard]] bool IsReclaiming() const { return is_reclaiming_.load(std::memory_order_acquire); }

 private:
  std::mutex mutex_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> is_reclaiming_{false};
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_SPACE_RECLAIMER_H_
