// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_IINDEX_ORCHESTRATOR_H_
#define SOEASY_SRC_SERVICE_IINDEX_ORCHESTRATOR_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

enum class IndexTaskKind {
  kInitial = 0,
  kRebuild = 1,
  kIncremental = 2,
};

struct IndexTaskResult {
  IndexTaskKind kind = IndexTaskKind::kInitial;
  uint64_t generation = 0;
  size_t total_indexed = 0;
  bool success = false;
  bool cancelled = false;
  double elapsed_sec = 0.0;
  bool has_pending_followup = false;
};

using ScanProgressCallback = std::function<void(size_t indexed, size_t total, const std::string &current_path)>;
using IndexTaskStartedCallback = std::function<void(IndexTaskKind kind, uint64_t generation)>;
using IndexTaskProgressCallback =
    std::function<void(IndexTaskKind kind, uint64_t generation, size_t indexed, size_t total, const std::string &path)>;
using IndexTaskFinishedCallback = std::function<void(const IndexTaskResult &result)>;

struct BackgroundIndexCallbacks {
  IndexTaskStartedCallback on_started;
  IndexTaskProgressCallback on_progress;
  IndexTaskFinishedCallback on_finished;
};

/**
 * @brief Abstract contract for the CQRS command/ingestion orchestration mid-tier.
 *
 * Exclusively provides write and ingestion operations (file crawler scanning,
 * filesystem watcher, worker sandbox dispatch, async commit queue, and vacuum).
 */
class IIndexOrchestrator {
 public:
  IIndexOrchestrator() = default;
  virtual ~IIndexOrchestrator() = default;

  IIndexOrchestrator(const IIndexOrchestrator &) = delete;
  IIndexOrchestrator &operator=(const IIndexOrchestrator &) = delete;
  IIndexOrchestrator(IIndexOrchestrator &&) = delete;
  IIndexOrchestrator &operator=(IIndexOrchestrator &&) = delete;

  bool Init() { return DoInit(); }
  bool StartLiveWatch() { return DoStartLiveWatch(); }
  void StopLiveWatch() { DoStopLiveWatch(); }

  bool AddAndScanDirectory(const std::string &dir_path, bool recursive = true) {
    return DoAddAndScanDirectory(dir_path, recursive);
  }
  bool RemoveDirectoryIndex(const std::string &dir_path) { return DoRemoveDirectoryIndex(dir_path); }

  void TriggerAsyncSpaceReclaim() { DoTriggerAsyncSpaceReclaim(); }

  size_t PerformInitialScan(ScanProgressCallback progress_cb = nullptr) {
    return DoPerformInitialScan(std::move(progress_cb));
  }
  bool RebuildIndex(ScanProgressCallback progress_cb = nullptr) { return DoRebuildIndex(std::move(progress_cb)); }

  bool HandleFileChange(const DebouncedEvent &event) { return DoHandleFileChange(event); }
  void FlushWriteQueue() { DoFlushWriteQueue(); }

  void ApplyRuntimeConfig() { DoApplyRuntimeConfig(); }
  void SyncWatchedDirectories() { DoSyncWatchedDirectories(); }
  void StartBackgroundTask(bool rebuild, bool force_incremental, BackgroundIndexCallbacks callbacks) {
    DoStartBackgroundTask(rebuild, force_incremental, std::move(callbacks));
  }
  void ClearBackgroundCallbacks() { DoClearBackgroundCallbacks(); }
  [[nodiscard]] bool IsIndexing() const { return DoIsIndexing(); }
  [[nodiscard]] size_t IndexedFileCount() const { return DoIndexedFileCount(); }

  void CancelScan() { DoCancelScan(); }
  [[nodiscard]] bool is_scan_cancelled() const { return DoIsScanCancelled(); }

  [[nodiscard]] PipelineStats GetStats() const { return DoGetStats(); }

 protected:
  virtual bool DoInit() = 0;
  virtual bool DoStartLiveWatch() = 0;
  virtual void DoStopLiveWatch() = 0;

  virtual bool DoAddAndScanDirectory(const std::string &dir_path, bool recursive) = 0;
  virtual bool DoRemoveDirectoryIndex(const std::string &dir_path) = 0;

  virtual void DoTriggerAsyncSpaceReclaim() = 0;

  virtual size_t DoPerformInitialScan(ScanProgressCallback progress_cb) = 0;
  virtual bool DoRebuildIndex(ScanProgressCallback progress_cb) = 0;

  virtual bool DoHandleFileChange(const DebouncedEvent &event) = 0;
  virtual void DoFlushWriteQueue() = 0;

  virtual void DoApplyRuntimeConfig() = 0;
  virtual void DoSyncWatchedDirectories() = 0;
  virtual void DoStartBackgroundTask(bool rebuild, bool force_incremental, BackgroundIndexCallbacks callbacks) = 0;
  virtual void DoClearBackgroundCallbacks() = 0;
  [[nodiscard]] virtual bool DoIsIndexing() const = 0;
  [[nodiscard]] virtual size_t DoIndexedFileCount() const = 0;

  virtual void DoCancelScan() = 0;
  [[nodiscard]] virtual bool DoIsScanCancelled() const = 0;

  [[nodiscard]] virtual PipelineStats DoGetStats() const = 0;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_IINDEX_ORCHESTRATOR_H_
