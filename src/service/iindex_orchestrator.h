// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_IINDEX_ORCHESTRATOR_H_
#define SOEASY_SRC_SERVICE_IINDEX_ORCHESTRATOR_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "service/event_debouncer.h"

namespace soeasy {
namespace service {

struct PipelineStats {
  uint64_t total_files_indexed = 0;
  uint64_t total_bytes_indexed = 0;
  uint64_t total_files_deleted = 0;
  uint64_t total_parse_failures = 0;
};

using ScanProgressCallback = std::function<void(size_t indexed, size_t total, const std::string &current_path)>;

/**
 * @brief Abstract contract for the CQRS command/ingestion orchestration mid-tier.
 *
 * Exclusively provides write and ingestion operations (file crawler scanning,
 * filesystem watcher, worker sandbox dispatch, async commit queue, and vacuum).
 */
class IIndexOrchestrator {
 public:
  virtual ~IIndexOrchestrator() = default;

  virtual bool Init() = 0;
  virtual bool StartLiveWatch() = 0;
  virtual void StopLiveWatch() = 0;

  virtual bool AddAndScanDirectory(const std::string &dir_path, bool recursive = true) = 0;
  virtual bool RemoveDirectoryIndex(const std::string &dir_path) = 0;

  virtual void TriggerAsyncSpaceReclaim() = 0;

  virtual size_t PerformInitialScan(ScanProgressCallback progress_cb = nullptr) = 0;
  virtual bool RebuildIndex(ScanProgressCallback progress_cb = nullptr) = 0;

  virtual bool HandleFileChange(const DebouncedEvent &event) = 0;
  virtual void FlushWriteQueue() = 0;

  virtual void CancelScan() = 0;
  [[nodiscard]] virtual bool is_scan_cancelled() const = 0;

  [[nodiscard]] virtual PipelineStats GetStats() const = 0;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_IINDEX_ORCHESTRATOR_H_
