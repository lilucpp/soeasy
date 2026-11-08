// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_INDEX_ORCHESTRATOR_H_
#define SOEASY_SRC_SERVICE_INDEX_ORCHESTRATOR_H_

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
#include "storage/storage_engine.h"

namespace soeasy {
namespace service {

/**
 * @brief CQRS Command-side implementation of IndexOrchestrator.
 *
 * Exclusively responsible for filesystem monitoring, crawler scanning, sandbox parsing dispatch,
 * asynchronous WAL write queues, incremental change handling, and space reclamation.
 */
class IndexOrchestrator : public IIndexOrchestrator {
 public:
  IndexOrchestrator(std::shared_ptr<IndexConfig> config, std::shared_ptr<WorkerGuard> worker_guard,
                    std::shared_ptr<storage::StorageEngine> storage_engine,
                    std::shared_ptr<NameIndex> name_index);
  ~IndexOrchestrator() override;

  IndexOrchestrator(const IndexOrchestrator &) = delete;
  IndexOrchestrator &operator=(const IndexOrchestrator &) = delete;

  bool Init() override;
  bool StartLiveWatch() override;
  void StopLiveWatch() override;

  bool AddAndScanDirectory(const std::string &dir_path, bool recursive = true) override;
  bool RemoveDirectoryIndex(const std::string &dir_path) override;

  void TriggerAsyncSpaceReclaim() override;

  size_t PerformInitialScan(ScanProgressCallback progress_cb = nullptr) override;
  bool RebuildIndex(ScanProgressCallback progress_cb = nullptr) override;

  bool HandleFileChange(const DebouncedEvent &event) override;
  void FlushWriteQueue() override;

  void CancelScan() override { cancel_scan_.store(true); }
  [[nodiscard]] bool is_scan_cancelled() const override { return cancel_scan_.load(); }

  [[nodiscard]] PipelineStats GetStats() const override;

  // Static document ID generator
  static int64_t GenerateDocId(const std::string &path);

  // Accessors
  std::shared_ptr<IndexConfig> config() { return config_; }
  std::shared_ptr<WorkerGuard> worker_guard() { return worker_guard_; }
  std::shared_ptr<NameIndex> name_index() { return name_index_; }
  std::shared_ptr<WatchEngine> watch_engine() { return watch_engine_; }
  std::shared_ptr<storage::StorageEngine> storage_engine() { return storage_engine_; }

 private:
  void ScanDirectoryRecursive(const std::string &dir_path);
  bool IndexSingleFile(const std::string &file_path);
  void PurgeFailedFilesUnder(const std::string &dir_path);

  static constexpr int64_t kMinFreePagesForVacuum = 100;
  static constexpr double kMinFragRatioForVacuum = 0.15;
  static constexpr int kVacuumChunkPages = 250;
  static constexpr std::chrono::milliseconds kVacuumPacingInterval{20};

  std::shared_ptr<IndexConfig> config_;
  std::shared_ptr<WorkerGuard> worker_guard_;
  std::shared_ptr<storage::StorageEngine> storage_engine_;
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

  std::atomic<bool> is_reclaiming_space_{false};
  std::thread space_reclaim_thread_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_INDEX_ORCHESTRATOR_H_
