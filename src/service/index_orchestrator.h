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
#include <unordered_set>
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
  void DoApplyConfigChanges(const ConfigChangeSummary &diff, BackgroundIndexCallbacks callbacks) override;
  size_t DoPruneStaleEntriesByConfig() override;
  void DoSyncWatchedDirectories() override;
  void DoStartBackgroundTask(bool rebuild, bool force_incremental, BackgroundIndexCallbacks callbacks) override;
  void DoClearBackgroundCallbacks() override;
  [[nodiscard]] bool DoIsIndexing() const override;
  [[nodiscard]] OrchestratorPhase DoGetPhase() const override { return phase_.load(std::memory_order_acquire); }
  [[nodiscard]] bool DoHasIncompleteRebuild() const override {
    return rebuild_incomplete_.load(std::memory_order_acquire);
  }
  [[nodiscard]] size_t DoIndexedFileCount() const override;

  void DoCancelScan() override;
  [[nodiscard]] bool DoIsScanCancelled() const override { return cancel_scan_.load(); }

  [[nodiscard]] PipelineStats DoGetStats() const override;

 private:
  enum class PendingTask { kNone = 0, kIncremental = 1, kConfigSync = 2, kRebuild = 3 };

  static constexpr size_t kPathLockBuckets = 64;

  std::mutex &LockBucketForPath(const std::string &file_path) const;
  size_t PerformScanInternal(ScanProgressCallback progress_cb, bool manage_buffering = true, bool manage_phase = true,
                             bool force_ocr_reindex = false);
  void CollectScanCandidates(const std::vector<std::string> &watched_dirs, std::vector<std::string> *files_to_index,
                             std::unordered_set<int64_t> *visited_doc_ids,
                             std::vector<std::string> *traversed_root_prefixes);
  bool RebuildIndexInternal(ScanProgressCallback progress_cb, bool manage_phase = true);
  void SweepOfflineDeletedFiles(const std::unordered_set<int64_t> &visited_doc_ids,
                                const std::vector<std::string> &traversed_root_prefixes);
  void EnableWatchBuffering(bool clear_existing);
  void RecordBufferedEventUnlocked(const DebouncedEvent &event);
  void ReconcileAndDisableWatchBuffering();
  bool ApplyLiveFileChange(const DebouncedEvent &event, bool from_reconcile);
  size_t PruneStaleEntriesByConfigLocked();
  void SetRebuildIncomplete(bool incomplete);
  void SyncLiveWatchesWithoutStop();
  void StopAndJoinReclaim();
  void StopAndJoinBackgroundTask();
  void EnqueueOrLaunchBackgroundTask(IndexTaskKind initial_kind, PendingTask req_pending,
                                     ConfigChangeSummary req_diff, bool cancel_active_non_rebuild,
                                     BackgroundIndexCallbacks callbacks);
  void BackgroundTaskLoop(IndexTaskKind initial_kind, uint64_t initial_generation, PendingTask initial_task_mode,
                          ConfigChangeSummary initial_diff, bool initial_force_rescan);
  void ScanDirectoryRecursive(const std::string &dir_path, bool force_reindex = false);
  bool IndexSingleFile(const std::string &file_path, bool force_reindex = false);
  bool IndexSingleFileLocked(const std::string &file_path, bool force_reindex = false);
  void PersistIndexedFile(int64_t doc_id, const std::string &file_path, const std::string &filename,
                          const std::string &ext, uint64_t file_size, int64_t file_mtime, std::string content);
  void PurgeFailedFilesUnder(const std::string &dir_path);

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
  IndexTaskKind active_kind_{IndexTaskKind::kInitial};
  std::atomic<bool> bg_stopping_{false};
  PendingTask pending_task_{PendingTask::kNone};
  ConfigChangeSummary pending_diff_;
  bool pending_requires_scan_{false};
  std::atomic<uint64_t> bg_generation_{0};
  std::atomic<OrchestratorPhase> phase_{OrchestratorPhase::kIdle};
  std::atomic<bool> rebuild_incomplete_{false};
  BackgroundIndexCallbacks bg_callbacks_;
  std::thread bg_task_thread_;

  // Watch-first buffering during scan/rebuild -> reconciled upon completion.
  std::mutex live_dispatch_mutex_;
  std::mutex buffered_events_mutex_;
  std::atomic<bool> buffer_watch_events_{false};
  std::unordered_map<std::string, DebouncedEvent> buffered_watch_events_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_INDEX_ORCHESTRATOR_H_
