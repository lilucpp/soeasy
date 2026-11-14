// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_INDEX_PIPELINE_H_
#define SOEASY_SRC_SERVICE_INDEX_PIPELINE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "service/iindex_orchestrator.h"
#include "service/index_config.h"
#include "service/isearch_coordinator.h"
#include "service/name_index.h"

namespace soeasy {

namespace storage {
class IStorageEngine;
}  // namespace storage

namespace service {

class WorkerGuard;

/**
 * @brief Unified Facade aggregating CQRS Command (IndexOrchestrator) and Query (SearchCoordinator).
 *
 * Maintains 100% backward compatibility for existing callers while delegating internally
 * to decoupled Command/Query coordinators.
 */
class IndexPipeline {
 public:
  IndexPipeline(std::shared_ptr<IndexConfig> config, std::shared_ptr<WorkerGuard> worker_guard,
                std::shared_ptr<storage::IStorageEngine> storage_engine);
  ~IndexPipeline();

  IndexPipeline(const IndexPipeline &) = delete;
  IndexPipeline &operator=(const IndexPipeline &) = delete;

  bool Init();
  bool StartLiveWatch();
  void StopLiveWatch();

  bool AddAndScanDirectory(const std::string &dir_path, bool recursive = true);
  bool RemoveDirectoryIndex(const std::string &dir_path);

  void TriggerAsyncSpaceReclaim();

  size_t PerformInitialScan(ScanProgressCallback progress_cb = nullptr);
  bool RebuildIndex(ScanProgressCallback progress_cb = nullptr);

  bool HandleFileChange(const DebouncedEvent &event);

  // CQRS Query Delegations
  std::vector<NameMatchResult> SearchFilename(const std::string &query, int limit = 50, int offset = 0,
                                              const std::string &path_prefix = "",
                                              int *out_total_count = nullptr) const;

  std::vector<SearchResultItem> SearchFulltext(const std::string &query, int limit = 50, int offset = 0,
                                               const std::string &path_prefix = "", bool sync_flush = true,
                                               int *out_total_count = nullptr);

  bool GetDocumentContent(int64_t doc_id, std::string *out_content) const;

  void FlushWriteQueue();

  void ApplyRuntimeConfig();
  void ApplyConfigChanges(const ConfigChangeSummary &diff, BackgroundIndexCallbacks callbacks = {});
  size_t PruneStaleEntriesByConfig();
  void SyncWatchedDirectories();
  void StartBackgroundTask(bool rebuild, bool force_incremental, BackgroundIndexCallbacks callbacks);
  void ClearBackgroundCallbacks();
  [[nodiscard]] bool IsIndexing() const;
  [[nodiscard]] OrchestratorPhase GetPhase() const;
  [[nodiscard]] bool HasIncompleteRebuild() const;
  [[nodiscard]] size_t IndexedFileCount() const;

  void CancelScan();
  [[nodiscard]] bool is_scan_cancelled() const;

  // Accessors
  std::shared_ptr<IndexConfig> config() { return config_; }
  std::shared_ptr<NameIndex> name_index() { return name_index_; }

  std::shared_ptr<ISearchCoordinator> search_coordinator() { return search_coordinator_; }
  std::shared_ptr<IIndexOrchestrator> orchestrator() { return orchestrator_; }

  [[nodiscard]] PipelineStats GetStats() const;

  static int64_t GenerateDocId(const std::string &path);

 private:
  std::shared_ptr<IndexConfig> config_;
  std::shared_ptr<WorkerGuard> worker_guard_;
  std::shared_ptr<storage::IStorageEngine> storage_engine_;
  std::shared_ptr<NameIndex> name_index_;

  std::shared_ptr<IIndexOrchestrator> orchestrator_;
  std::shared_ptr<ISearchCoordinator> search_coordinator_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_INDEX_PIPELINE_H_
