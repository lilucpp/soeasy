#include "service/index_pipeline.h"

#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "service/doc_prechecker.h"

namespace soeasy {
namespace service {

namespace {

std::string ExtractFileName(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

std::string ExtractExtension(const std::string& filename) {
  size_t dot_pos = filename.find_last_of('.');
  if (dot_pos == std::string::npos || dot_pos == 0) {
    return "";
  }
  std::string ext = filename.substr(dot_pos + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext;
}

}  // namespace

int64_t IndexPipeline::GenerateDocId(const std::string& path) {
  constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
  constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
  uint64_t hash = kFnvOffsetBasis;
  for (char c : path) {
    hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
    hash *= kFnvPrime;
  }
  // Ensure positive signed 64-bit integer
  return static_cast<int64_t>(hash & 0x7FFFFFFFFFFFFFFFLL);
}

IndexPipeline::IndexPipeline(
    std::shared_ptr<IndexConfig> config,
    std::shared_ptr<WorkerGuard> worker_guard,
    std::shared_ptr<storage::StorageEngine> storage_engine)
    : config_(std::move(config)),
      worker_guard_(std::move(worker_guard)),
      storage_engine_(std::move(storage_engine)) {}

IndexPipeline::~IndexPipeline() {
  StopLiveWatch();
}

bool IndexPipeline::Init() {
  if (!config_ || !worker_guard_ || !storage_engine_) {
    return false;
  }

  debouncer_ = std::make_shared<EventDebouncer>(config_->debounce_ms());
  debouncer_->SetCallback([this](const DebouncedEvent& ev) {
    HandleFileChange(ev);
  });

  name_index_ = std::make_shared<NameIndex>();

  // Cold-start recovery: load existing files metadata directly from database into NameIndex
  auto existing_files = storage_engine_->GetAllFilesMetadata();
  for (const auto& f : existing_files) {
    NameRecord rec;
    rec.doc_id = f.doc_id;
    rec.file_name = f.file_name;
    rec.file_path = f.file_path;
    rec.ext = f.ext;
    rec.file_size = f.file_size;
    rec.mtime = f.mtime;
    name_index_->Upsert(rec);
  }
  LOG_INFO("[IndexPipeline] Cold-started NameIndex with {} files from database.",
           existing_files.size());

  watch_engine_ = std::make_shared<WatchEngine>(config_, debouncer_);
  write_queue_ = std::make_shared<AsyncWriteQueue>();
  write_queue_->Start(storage_engine_, 100, std::chrono::seconds(10), 500);
  return true;
}

bool IndexPipeline::StartLiveWatch() {
  if (!watch_engine_) {
    return false;
  }
  return watch_engine_->Start();
}

void IndexPipeline::StopLiveWatch() {
  if (watch_engine_) {
    watch_engine_->Stop();
  }
  if (debouncer_) {
    debouncer_->Stop(true);
  }
  if (write_queue_) {
    write_queue_->Stop();
  }
}

void IndexPipeline::FlushWriteQueue() {
  if (write_queue_) {
    write_queue_->Flush();
  }
}

bool IndexPipeline::AddAndScanDirectory(const std::string& dir_path,
                                        bool recursive) {
  if (!watch_engine_) {
    return false;
  }

  LOG_INFO("[IndexPipeline] Adding directory to monitor and scan: '{}'", dir_path);

  // 1. Register with watch engine for live events
  watch_engine_->AddWatch(dir_path);

  // 2. Perform baseline crawl of existing files
  ScanDirectoryRecursive(dir_path);
  FlushWriteQueue();
  return true;
}

void IndexPipeline::ScanDirectoryRecursive(const std::string& dir_path) {
  try {
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir_path, std::filesystem::directory_options::skip_permission_denied);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (it->is_directory()) {
        std::string sub_dir = it->path().string();
        if (config_ && config_->IsDirectoryExcluded(sub_dir)) {
          it.disable_recursion_pending();
          continue;
        }
      }
      if (it->is_regular_file()) {
        std::string file_path = it->path().string();
        std::string filename = it->path().filename().string();
        std::string ext = ExtractExtension(filename);

        uint64_t fsize = 0;
        try {
          fsize = it->file_size();
        } catch (...) {
          fsize = 0;
        }

        if (config_->ShouldIndex(file_path, ext, fsize)) {
          IndexSingleFile(file_path);
        }
      }
    }
  } catch (const std::exception& e) {
    LOG_WARN("[IndexPipeline] Directory scan error on '{}': {}", dir_path, e.what());
  }
}

size_t IndexPipeline::PerformInitialScan(
    std::function<void(size_t indexed, size_t total, const std::string& path)> progress_cb) {
  auto watched_dirs = config_->GetIndexedDirectories();
  if (watched_dirs.empty() && watch_engine_) {
    watched_dirs = watch_engine_->GetWatchedDirectories();
  }

  // 1. Register watches for live filesystem events
  if (watch_engine_) {
    for (const auto& dir : watched_dirs) {
      watch_engine_->AddWatch(dir);
    }
  }

  // 2. Discover all candidate files
  std::vector<std::string> files_to_index;
  for (const auto& dir : watched_dirs) {
    try {
      for (auto it = std::filesystem::recursive_directory_iterator(
               dir, std::filesystem::directory_options::skip_permission_denied);
           it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
          std::string sub_dir = it->path().string();
          if (config_ && config_->IsDirectoryExcluded(sub_dir)) {
            it.disable_recursion_pending();
            continue;
          }
        }
        if (it->is_regular_file()) {
          std::string fpath = it->path().string();
          std::string fname = it->path().filename().string();
          std::string ext = ExtractExtension(fname);
          uint64_t fsize = 0;
          try {
            fsize = it->file_size();
          } catch (...) {}
          if (config_->ShouldIndex(fpath, ext, fsize)) {
            files_to_index.push_back(fpath);
          }
        }
      }
    } catch (const std::exception& e) {
      LOG_WARN("[IndexPipeline] Initial scan error on '{}': {}", dir, e.what());
    }
  }

  size_t total_files = files_to_index.size();
  LOG_INFO("[IndexPipeline] Initial scan discovered {} files to index across {} directories.",
           total_files, watched_dirs.size());

  size_t indexed_count = 0;
  size_t processed_count = 0;

  if (storage_engine_) {
    storage_engine_->SetBulkMode(true);
  }

  cancel_scan_.store(false);
  for (const auto& fpath : files_to_index) {
    if (cancel_scan_.load()) {
      LOG_INFO("[IndexPipeline] Initial scan cancelled.");
      break;
    }
    if (progress_cb) {
      progress_cb(processed_count, total_files, fpath);
    }
    bool ok = IndexSingleFile(fpath);
    if (ok) {
      indexed_count++;
    }
    processed_count++;
  }

  FlushWriteQueue();

  if (storage_engine_) {
    storage_engine_->SetBulkMode(false);
  }

  if (progress_cb) {
    progress_cb(total_files, total_files, "");
  }

  return indexed_count;
}

bool IndexPipeline::RebuildIndex(
    std::function<void(size_t indexed, size_t total, const std::string& path)> progress_cb) {
  LOG_INFO("[IndexPipeline] Starting full index rebuild from scratch...");
  auto start_time = std::chrono::high_resolution_clock::now();

  // 1. Temporarily pause live watcher and drain debouncer
  bool was_watching = false;
  if (watch_engine_) {
    was_watching = watch_engine_->is_watching();
    watch_engine_->Stop();
  }

  // 2. Clear in-memory filename index
  if (name_index_) {
    name_index_->Clear();
    LOG_INFO("[IndexPipeline] Cleared in-memory NameIndex.");
  }

  // 3. Reset persistent storage and FTS tables
  FlushWriteQueue();
  if (storage_engine_) {
    if (!storage_engine_->ClearAll()) {
      LOG_ERROR("[IndexPipeline] Failed to reset storage engine tables: {}",
                storage_engine_->GetLastError());
      return false;
    }
    LOG_INFO("[IndexPipeline] Cleared persistent storage and FTS5 index.");
  }

  // 4. Reset stats & failed files cache
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = PipelineStats();
  }
  {
    std::lock_guard<std::mutex> lock(failed_files_mutex_);
    failed_files_.clear();
  }

  // 5. Gather all configured directories
  auto watched_dirs = config_->GetIndexedDirectories();
  if (watched_dirs.empty() && watch_engine_) {
    watched_dirs = watch_engine_->GetWatchedDirectories();
  }

  // Collect candidate files first for progress calculation
  std::vector<std::string> files_to_index;
  for (const auto& dir : watched_dirs) {
    try {
      for (auto it = std::filesystem::recursive_directory_iterator(
               dir, std::filesystem::directory_options::skip_permission_denied);
           it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
          std::string sub_dir = it->path().string();
          if (config_ && config_->IsDirectoryExcluded(sub_dir)) {
            it.disable_recursion_pending();
            continue;
          }
        }
        if (it->is_regular_file()) {
          std::string fpath = it->path().string();
          std::string fname = it->path().filename().string();
          std::string ext = ExtractExtension(fname);
          uint64_t fsize = 0;
          try {
            fsize = it->file_size();
          } catch (...) {}

          if (config_->ShouldIndex(fpath, ext, fsize)) {
            files_to_index.push_back(fpath);
          }
        }
      }
    } catch (const std::exception& e) {
      LOG_WARN("[IndexPipeline] Error enumerating directory '{}': {}", dir, e.what());
    }
  }

  size_t total_files = files_to_index.size();
  LOG_INFO("[IndexPipeline] Rebuild discovered {} files to index across {} directories.",
           total_files, watched_dirs.size());

  size_t indexed_count = 0;
  size_t processed_count = 0;

  if (storage_engine_) {
    storage_engine_->SetBulkMode(true);
  }

  cancel_scan_.store(false);
  for (const auto& fpath : files_to_index) {
    if (cancel_scan_.load()) {
      LOG_INFO("[IndexPipeline] Rebuild cancelled.");
      break;
    }
    bool ok = IndexSingleFile(fpath);
    if (ok) {
      indexed_count++;
    }
    processed_count++;
    if (progress_cb) {
      progress_cb(processed_count, total_files, fpath);
    }
  }

  FlushWriteQueue();

  // Optimize FTS5 B-tree segments after full rebuild
  if (storage_engine_) {
    storage_engine_->OptimizeFtsIndex();
    storage_engine_->SetBulkMode(false);
  }

  // 6. Restore watcher if it was running
  if (was_watching && watch_engine_) {
    watch_engine_->Start();
    for (const auto& dir : watched_dirs) {
      watch_engine_->AddWatch(dir);
    }
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  double elapsed_ms =
      std::chrono::duration<double, std::milli>(end_time - start_time).count();

  LOG_INFO(
      "[IndexPipeline] Index rebuild finished in {:.2f} ms | Indexed: {} / {} files | "
      "Total parsed bytes: {}",
      elapsed_ms, indexed_count, total_files, stats_.total_bytes_indexed);

  return true;
}

bool IndexPipeline::IndexSingleFile(const std::string& file_path) {
  auto start_time = std::chrono::high_resolution_clock::now();

  struct stat st;
  if (stat(file_path.c_str(), &st) != 0) {
    LOG_WARN("[IndexPipeline] Failed to stat file: '{}'", file_path);
    return false;
  }

  std::string filename = ExtractFileName(file_path);
  std::string ext = ExtractExtension(filename);
  uint64_t file_size = static_cast<uint64_t>(st.st_size);

  if (!config_->ShouldIndex(file_path, ext, file_size)) {
    return false;
  }

  // Check if previously marked as failed/corrupt and file has not changed
  {
    std::lock_guard<std::mutex> lock(failed_files_mutex_);
    auto it = failed_files_.find(file_path);
    if (it != failed_files_.end() && it->second.file_size == file_size &&
        it->second.mtime == st.st_mtime) {
      return false;
    }
  }

  int64_t doc_id = GenerateDocId(file_path);

  // Fast incremental check: first check in-memory NameIndex (zero I/O), then fallback to StorageEngine
  uint64_t db_size = 0;
  int64_t db_mtime = 0;
  bool meta_found = false;
  if (name_index_ && name_index_->GetMetadata(doc_id, &db_size, &db_mtime)) {
    meta_found = true;
  } else if (storage_engine_ && storage_engine_->GetFileMetadata(doc_id, &db_size, &db_mtime)) {
    meta_found = true;
  }

  if (meta_found && db_size == file_size && db_mtime == st.st_mtime) {
    NameRecord rec;
    rec.doc_id = doc_id;
    rec.file_name = filename;
    rec.file_path = file_path;
    rec.ext = ext;
    rec.file_size = file_size;
    rec.mtime = st.st_mtime;
    name_index_->Upsert(rec);
    return true;
  }

  // 1. Fast sanity check (magic, EOCD, encryption precheck, dynamic timeout)
  auto precheck = DocPrechecker::Check(file_path, ext, file_size);
  if (precheck.status != PrecheckStatus::kValid) {
    LOG_WARN("[IndexPipeline] Precheck skipped body for '{}' (ext: '{}') | reason: {}",
             file_path, ext, precheck.reason);
    // Index filename into NameIndex and StorageEngine with empty body
    NameRecord rec;
    rec.doc_id = doc_id;
    rec.file_name = filename;
    rec.file_path = file_path;
    rec.ext = ext;
    rec.file_size = file_size;
    rec.mtime = st.st_mtime;
    name_index_->Upsert(rec);

    if (write_queue_ && write_queue_->is_running()) {
      WriteOp op{WriteOpType::kIndex, doc_id, file_path, filename, ext,
                 file_size, st.st_mtime, 1, ""};
      write_queue_->Enqueue(std::move(op));
    } else if (storage_engine_) {
      storage_engine_->IndexDocument(doc_id, file_path, filename, ext, file_size,
                                     st.st_mtime, 1, "");
    }

    std::lock_guard<std::mutex> lock(failed_files_mutex_);
    failed_files_[file_path] = {file_size, st.st_mtime, precheck.reason};
    return true;
  }

  // 2. Dispatch parsing task to worker sandbox pool with dynamic timeout
  ParseTask task{doc_id, file_path, ext, 1, precheck.recommended_timeout_ms};
  ParseTaskResult res;

  auto parse_start = std::chrono::high_resolution_clock::now();
  bool ok = worker_guard_->ExecuteTask(task, &res);
  auto parse_end = std::chrono::high_resolution_clock::now();
  double parse_cost_ms =
      std::chrono::duration<double, std::milli>(parse_end - parse_start).count();

  if (!ok || res.status != TaskStatus::kSuccess) {
    if (res.slot_id > 0) {
      worker_guard_->shm_pool()->ReleaseSlot(res.slot_id);
    }
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_parse_failures++;
    LOG_ERROR(
        "[Parser] FAILED to parse '{}' (ext: '{}', size: {} B) in {:.2f} ms | "
        "status: {} | err: {}",
        file_path, ext, file_size, parse_cost_ms, static_cast<int>(res.status),
        res.error_message);

    // Index filename into NameIndex and StorageEngine so it remains searchable
    NameRecord rec;
    rec.doc_id = doc_id;
    rec.file_name = filename;
    rec.file_path = file_path;
    rec.ext = ext;
    rec.file_size = file_size;
    rec.mtime = st.st_mtime;
    name_index_->Upsert(rec);

    if (write_queue_ && write_queue_->is_running()) {
      WriteOp op{WriteOpType::kIndex, doc_id, file_path, filename, ext,
                 file_size, st.st_mtime, 1, ""};
      write_queue_->Enqueue(std::move(op));
    } else if (storage_engine_) {
      storage_engine_->IndexDocument(doc_id, file_path, filename, ext, file_size,
                                     st.st_mtime, 1, "");
    }

    std::lock_guard<std::mutex> flock(failed_files_mutex_);
    failed_files_[file_path] = {file_size, st.st_mtime, res.error_message};
    return false;
  }

  // 2. Read extracted plain text from shared memory slot
  const uint8_t* extracted_data = nullptr;
  size_t extracted_bytes = 0;
  bool read_ok = worker_guard_->shm_pool()->ReadSlot(
      res.slot_id, &extracted_data, &extracted_bytes);

  std::string text_content;
  if (read_ok && extracted_data && extracted_bytes > 0) {
    text_content.assign(reinterpret_cast<const char*>(extracted_data),
                        extracted_bytes);
  }
  worker_guard_->shm_pool()->ReleaseSlot(res.slot_id);

  // 3. Update in-memory filename index (for instant filename queries)
  NameRecord rec;
  rec.doc_id = doc_id;
  rec.file_name = filename;
  rec.file_path = file_path;
  rec.ext = ext;
  rec.file_size = file_size;
  rec.mtime = st.st_mtime;
  name_index_->Upsert(rec);

  // 4. Update encrypted persistent database via async write queue
  if (write_queue_ && write_queue_->is_running()) {
    WriteOp op{WriteOpType::kIndex, doc_id, file_path, filename, ext,
               file_size, st.st_mtime, 1, std::move(text_content)};
    write_queue_->Enqueue(std::move(op));
  } else if (storage_engine_) {
    storage_engine_->IndexDocument(
        doc_id, file_path, filename, ext, file_size, st.st_mtime, 1, text_content);
  }

  auto total_end = std::chrono::high_resolution_clock::now();
  double total_cost_ms =
      std::chrono::duration<double, std::milli>(total_end - start_time).count();

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_files_indexed++;
    stats_.total_bytes_indexed += extracted_bytes;
  }

  LOG_INFO(
      "[Parser] SUCCESS parsed & enqueued '{}' (ext: '{}', size: {} B, extracted: {} B) | "
      "parse: {:.2f} ms, total: {:.2f} ms",
      file_path, ext, file_size, extracted_bytes, parse_cost_ms, total_cost_ms);

  return true;
}

bool IndexPipeline::HandleFileChange(const DebouncedEvent& event) {
  int64_t doc_id = GenerateDocId(event.file_path);

  if (event.action == FileChangeAction::kDelete) {
    LOG_INFO("[Watcher] File delete event: '{}' | removing from index", event.file_path);
    name_index_->Remove(doc_id);
    if (write_queue_ && write_queue_->is_running()) {
      WriteOp op;
      op.type = WriteOpType::kDelete;
      op.doc_id = doc_id;
      write_queue_->Enqueue(std::move(op));
    } else if (storage_engine_) {
      storage_engine_->DeleteDocument(doc_id);
    }

    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_files_deleted++;
    return true;
  }

  if (event.action == FileChangeAction::kAdd) {
    LOG_INFO("[Watcher] File added event: '{}' | triggering index", event.file_path);
    return IndexSingleFile(event.file_path);
  }

  if (event.action == FileChangeAction::kModified) {
    LOG_INFO("[Watcher] File modified event: '{}' | triggering re-index", event.file_path);
    return IndexSingleFile(event.file_path);
  }

  return true;
}

std::vector<NameMatchResult> IndexPipeline::SearchFilename(
    const std::string& query, int limit, int offset,
    const std::string& path_prefix,
    int* out_total_count) const {
  if (!name_index_) {
    if (out_total_count) {
      *out_total_count = 0;
    }
    return {};
  }
  return name_index_->Search(query, limit, offset, path_prefix, out_total_count);
}

std::vector<storage::SearchResultItem> IndexPipeline::SearchFulltext(
    const std::string& query, int limit, int offset,
    const std::string& path_prefix,
    bool sync_flush,
    int* out_total_count) {
  if (sync_flush) {
    FlushWriteQueue();
  }
  if (!storage_engine_) {
    if (out_total_count) {
      *out_total_count = 0;
    }
    return {};
  }

  return storage_engine_->Search(query, limit, offset, path_prefix, out_total_count);
}

PipelineStats IndexPipeline::GetStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

}  // namespace service
}  // namespace soeasy
