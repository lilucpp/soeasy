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
#include "service/index_orchestrator.h"
#include "service/isearch_coordinator.h"
#include "service/name_index.h"
#include "service/search_coordinator.h"
#include "service/watch_engine.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"

namespace soeasy {
namespace service {

/**
 * @brief Unified Facade aggregating CQRS Command (IndexOrchestrator) and Query (SearchCoordinator).
 *
 * Maintains 100% backward compatibility for existing callers while delegating internally
 * to decoupled Command/Query coordinators.
 */
class IndexPipeline {
 public:
  IndexPipeline(std::shared_ptr<IndexConfig> config, std::shared_ptr<WorkerGuard> worker_guard,
                std::shared_ptr<storage::StorageEngine> storage_engine);
  ~IndexPipeline() = default;

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

  std::vector<storage::SearchResultItem> SearchFulltext(const std::string &query, int limit = 50, int offset = 0,
                                                        const std::string &path_prefix = "", bool sync_flush = true,
                                                        int *out_total_count = nullptr);

  void FlushWriteQueue();

  void CancelScan();
  [[nodiscard]] bool is_scan_cancelled() const;

  // Accessors
  std::shared_ptr<IndexConfig> config() { return config_; }
  std::shared_ptr<WorkerGuard> worker_guard() { return worker_guard_; }
  std::shared_ptr<NameIndex> name_index() { return name_index_; }
  std::shared_ptr<WatchEngine> watch_engine() { return orchestrator_ ? orchestrator_->watch_engine() : nullptr; }
  std::shared_ptr<storage::StorageEngine> storage_engine() { return storage_engine_; }

  std::shared_ptr<ISearchCoordinator> search_coordinator() { return search_coordinator_; }
  std::shared_ptr<IIndexOrchestrator> orchestrator() { return orchestrator_; }

  [[nodiscard]] PipelineStats GetStats() const;

  static int64_t GenerateDocId(const std::string &path) {
    return IndexOrchestrator::GenerateDocId(path);
  }

 private:
  std::shared_ptr<IndexConfig> config_;
  std::shared_ptr<WorkerGuard> worker_guard_;
  std::shared_ptr<storage::StorageEngine> storage_engine_;
  std::shared_ptr<NameIndex> name_index_;

  std::shared_ptr<IndexOrchestrator> orchestrator_;
  std::shared_ptr<SearchCoordinator> search_coordinator_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_INDEX_PIPELINE_H_
