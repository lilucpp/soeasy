// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_WATCH_RECONCILER_H_
#define SOEASY_SRC_SERVICE_WATCH_RECONCILER_H_

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "service/event_debouncer.h"

namespace soeasy {
namespace service {

/**
 * @brief Watch-first event buffering and reconciliation manager.
 *
 * Captures and coalesces real-time filesystem events that arrive while initial scan
 * or full rebuild is actively traversing disk, ensuring zero lost events and zero race conditions.
 */
class WatchReconciler {
 public:
  WatchReconciler() = default;
  ~WatchReconciler() = default;

  // Non-copyable, non-movable
  WatchReconciler(const WatchReconciler &) = delete;
  WatchReconciler &operator=(const WatchReconciler &) = delete;

  void EnableBuffering(bool clear_existing);
  void RecordEvent(const DebouncedEvent &event);
  [[nodiscard]] bool IsBufferingEnabled() const;
  std::vector<DebouncedEvent> DrainBatch();
  bool TryDisableBufferingIfEmpty(std::vector<DebouncedEvent> *out_remaining);
  void ForceDisableBuffering();

 private:
  mutable std::mutex mutex_;
  std::atomic<bool> is_buffering_{false};
  std::unordered_map<std::string, DebouncedEvent> buffered_events_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_WATCH_RECONCILER_H_
