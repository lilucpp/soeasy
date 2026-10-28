#ifndef SOEASY_SRC_SERVICE_INDEX_PIPELINE_H_
#define SOEASY_SRC_SERVICE_INDEX_PIPELINE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "service/async_write_queue.h"
#include "service/event_debouncer.h"
#include "service/index_config.h"
#include "service/name_index.h"
#include "service/watch_engine.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"

namespace soeasy {
namespace service {

struct PipelineStats {
  uint64_t total_files_indexed = 0;
  uint64_t total_bytes_indexed = 0;
  uint64_t total_files_deleted = 0;
  uint64_t total_parse_failures = 0;
};

class IndexPipeline {
 public:
  IndexPipeline(std::shared_ptr<IndexConfig> config,
                std::shared_ptr<WorkerGuard> worker_guard,
                std::shared_ptr<storage::StorageEngine> storage_engine);
  ~IndexPipeline();

  // Disallow copy and assign
  IndexPipeline(const IndexPipeline&) = delete;
  IndexPipeline& operator=(const IndexPipeline&) = delete;

  /**
   * @brief Initialize debouncer, name index, and watcher components.
   */
  bool Init();

  /**
   * @brief Start live filesystem monitoring.
   */
  bool StartLiveWatch();

  /**
   * @brief Stop monitoring and wait for event queue drain.
   */
  void StopLiveWatch();

  /**
   * @brief Add a directory to monitor and perform initial crawl/indexing.
   */
  bool AddAndScanDirectory(const std::string& dir_path, bool recursive = true);

  /**
   * @brief Perform initial background scan and indexing of configured directories.
   * Incremental: skips unchanged files without reprocessing.
   *
   * @param progress_cb Optional callback receiving (indexed_count, total_count, current_path)
   * @return Number of files indexed
   */
  size_t PerformInitialScan(std::function<void(size_t indexed, size_t total, const std::string& path)> progress_cb = nullptr);

  /**
   * @brief Rebuild full-text and filename indexes from scratch.
   * Clears database and in-memory index, then rescans all configured directories.
   *
   * @param progress_cb Optional callback receiving (indexed_count, total_count, current_path)
   * @return true on success
   */
  bool RebuildIndex(std::function<void(size_t indexed, size_t total, const std::string& path)> progress_cb = nullptr);

  /**
   * @brief Process a single settled file change event from debouncer.
   */
  bool HandleFileChange(const DebouncedEvent& event);

  /**
   * @brief Perform fast filename search via in-memory NameIndex.
   */
  std::vector<NameMatchResult> SearchFilename(
      const std::string& query, int limit = 50,
      const std::string& path_prefix = "") const;

  /**
   * @brief Perform encrypted full-text search via StorageEngine with optional path filter.
   */
  std::vector<storage::SearchResultItem> SearchFulltext(
      const std::string& query, int limit = 50, int offset = 0,
      const std::string& path_prefix = "");

  /**
   * @brief Block until all background write operations in the async queue are committed.
   */
  void FlushWriteQueue();

  // Accessors
  std::shared_ptr<IndexConfig> config() { return config_; }
  std::shared_ptr<WorkerGuard> worker_guard() { return worker_guard_; }
  std::shared_ptr<NameIndex> name_index() { return name_index_; }
  std::shared_ptr<WatchEngine> watch_engine() { return watch_engine_; }
  std::shared_ptr<storage::StorageEngine> storage_engine() { return storage_engine_; }
  PipelineStats GetStats() const;

  /**
   * @brief Compute canonical 64-bit document ID from filesystem path.
   */
  static int64_t GenerateDocId(const std::string& path);

 private:
  void ScanDirectoryRecursive(const std::string& dir_path);
  bool IndexSingleFile(const std::string& file_path);

  std::shared_ptr<IndexConfig> config_;
  std::shared_ptr<WorkerGuard> worker_guard_;
  std::shared_ptr<storage::StorageEngine> storage_engine_;

  std::shared_ptr<EventDebouncer> debouncer_;
  std::shared_ptr<WatchEngine> watch_engine_;
  std::shared_ptr<NameIndex> name_index_;
  std::shared_ptr<AsyncWriteQueue> write_queue_;

  mutable std::mutex stats_mutex_;
  PipelineStats stats_;

  struct FailedFileInfo {
    uint64_t file_size = 0;
    int64_t mtime = 0;
    std::string reason;
  };
  std::mutex failed_files_mutex_;
  std::unordered_map<std::string, FailedFileInfo> failed_files_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_INDEX_PIPELINE_H_
