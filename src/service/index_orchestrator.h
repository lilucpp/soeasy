// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_INDEX_ORCHESTRATOR_H_
#define SOEASY_SRC_SERVICE_INDEX_ORCHESTRATOR_H_

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "service/async_write_queue.h"
#include "service/event_debouncer.h"
#include "service/iindex_orchestrator.h"
#include "service/index_config.h"
#include "service/name_index.h"
#include "service/watch_engine.h"
#include "service/worker_guard.h"
#include "storage/istorage_engine.h"

namespace soeasy {
namespace service {

/**
 * @brief CQRS Command-side implementation of IndexOrchestrator.
 *
 * Exclusively responsible for filesystem monitoring, crawler scanning, sandbox parsing dispatch,
 * asynchronous WAL write queues, incremental change handling, and space reclamation.
 */
class IndexOrchestrator final : public IIndexOrchestrator {
 public:
  IndexOrchestrator(std::shared_ptr<IndexConfig> config, std::shared_ptr<WorkerGuard> worker_guard,
                    std::shared_ptr<storage::IStorageEngine> storage_engine, std::shared_ptr<NameIndex> name_index);
  ~IndexOrchestrator() override;

  // Static document ID generator
  static int64_t GenerateDocId(const std::string &path);

  // Accessors
  std::shared_ptr<IndexConfig> config() { return config_; }
  std::shared_ptr<NameIndex> name_index() { return name_index_; }

 protected:
  bool DoInit() override;
  bool DoStartLiveWatch() override;
  void DoStopLiveWatch() override;

  bool DoAddAndScanDirectory(const std::string &dir_path, bool recursive) override;
  bool DoRemoveDirectoryIndex(const std::string &dir_path) override;

  void DoTriggerAsyncSpaceReclaim() override;

  size_t DoPerformInitialScan(ScanProgressCallback progress_cb) override;
  bool DoRebuildIndex(ScanProgressCallback progress_cb) override;

  bool DoHandleFileChange(const DebouncedEvent &event) override;
  void DoFlushWriteQueue() override;

  void DoApplyRuntimeConfig() override;
  void DoSyncWatchedDirectories() override;
  void DoStartBackgroundTask(bool rebuild, bool force_incremental, BackgroundIndexCallbacks callbacks) override;
  void DoClearBackgroundCallbacks() override;
  [[nodiscard]] bool DoIsIndexing() const override;
  [[nodiscard]] size_t DoIndexedFileCount() const override;

  void DoCancelScan() override;
  [[nodiscard]] bool DoIsScanCancelled() const override { return cancel_scan_.load(); }

  [[nodiscard]] PipelineStats DoGetStats() const override;

 private:
  enum class PendingTask { kNone = 0, kIncremental = 1, kRebuild = 2 };

  static constexpr size_t kPathLockBuckets = 64;

  std::mutex &LockBucketForPath(const std::string &file_path) const;
  size_t PerformScanInternal(ScanProgressCallback progress_cb);
  void StopAndJoinReclaim();
  void StopAndJoinBackgroundTask();
  void BackgroundTaskLoop(IndexTaskKind initial_kind, uint64_t initial_generation);
  void ScanDirectoryRecursive(const std::string &dir_path);
  bool IndexSingleFile(const std::string &file_path);
  bool IndexSingleFileLocked(const std::string &file_path);
  void PersistIndexedFile(int64_t doc_id, const std::string &file_path, const std::string &filename,
                          const std::string &ext, uint64_t file_size, int64_t file_mtime, std::string content);
  void PurgeFailedFilesUnder(const std::string &dir_path);

  /**
   * @brief Re-register configured directories and restart the watcher after a maintenance pause.
   */
  void RestartLiveWatch(bool was_watching);

  static constexpr int64_t kMinFreePagesForVacuum = 100;
  static constexpr double kMinFragRatioForVacuum = 0.15;
  static constexpr int kVacuumChunkPages = 250;
  static constexpr std::chrono::milliseconds kVacuumPacingInterval{20};

  std::shared_ptr<IndexConfig> config_;
  std::shared_ptr<WorkerGuard> worker_guard_;
  std::shared_ptr<storage::IStorageEngine> storage_engine_;
  std::shared_ptr<NameIndex> name_index_;

  std::shared_ptr<EventDebouncer> debouncer_;
  std::shared_ptr<WatchEngine> watch_engine_;
  std::shared_ptr<AsyncWriteQueue> write_queue_;

  mutable std::mutex stats_mutex_;
  PipelineStats stats_;
  std::atomic<bool> cancel_scan_{false};

  struct FailedFileInfo {
    uint64_t file_size = 0;
    int64_t mtime = 0;
    std::string reason;
  };
  std::mutex failed_files_mutex_;
  std::unordered_map<std::string, FailedFileInfo> failed_files_;

  mutable std::array<std::mutex, kPathLockBuckets> path_locks_;
  std::mutex scan_mutex_;

  std::mutex reclaim_mutex_;
  std::atomic<bool> stop_reclaim_{false};
  std::atomic<bool> is_reclaiming_space_{false};
  std::thread space_reclaim_thread_;

  std::mutex bg_join_mutex_;
  mutable std::mutex bg_task_mutex_;
  std::mutex bg_callback_mutex_;
  bool bg_task_running_{false};
  std::atomic<bool> bg_stopping_{false};
  PendingTask pending_task_{PendingTask::kNone};
  std::atomic<uint64_t> bg_generation_{0};
  BackgroundIndexCallbacks bg_callbacks_;
  std::thread bg_task_thread_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_INDEX_ORCHESTRATOR_H_
