// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "service/index_orchestrator.h"

#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/encoding_utils.h"
#include "common/logger.h"
#include "common/resource_profile.h"
#include "service/doc_prechecker.h"

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

namespace soeasy {
namespace service {

namespace {

struct DirFingerprint {
  uint64_t dev{0};
  uint64_t ino{0};
  bool operator==(const DirFingerprint &other) const { return dev == other.dev && ino == other.ino; }
};

struct DirFingerprintHash {
  std::size_t operator()(const DirFingerprint &fp) const {
    return std::hash<uint64_t>()(fp.dev) ^ (std::hash<uint64_t>()(fp.ino) << 1);
  }
};

bool GetDirFingerprint(const std::filesystem::path &p, DirFingerprint *fp) {
#ifndef _WIN32
  struct stat st;
  if (::stat(p.c_str(), &st) == 0) {
    if (fp) {
      fp->dev = static_cast<uint64_t>(st.st_dev);
      fp->ino = static_cast<uint64_t>(st.st_ino);
    }
    return true;
  }
  return false;
#else
  HANDLE h_file = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h_file != INVALID_HANDLE_VALUE) {
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(h_file, &info)) {
      if (fp) {
        fp->dev = static_cast<uint64_t>(info.dwVolumeSerialNumber);
        fp->ino = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
      }
      CloseHandle(h_file);
      return true;
    }
    CloseHandle(h_file);
  }
  return false;
#endif
}

std::string ExtractFileName(const std::string &path) {
  size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

std::string ExtractExtension(const std::string &filename) {
  size_t dot_pos = filename.find_last_of('.');
  if (dot_pos == std::string::npos || dot_pos == 0) {
    return "";
  }
  std::string ext = filename.substr(dot_pos + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return ext;
}

}  // namespace

int64_t IndexOrchestrator::GenerateDocId(const std::string &path) {
  constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
  constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
  uint64_t hash = kFnvOffsetBasis;
  for (char c : path) {
    hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
    hash *= kFnvPrime;
  }
  return static_cast<int64_t>(hash & 0x7FFFFFFFFFFFFFFFLL);
}

IndexOrchestrator::IndexOrchestrator(std::shared_ptr<IndexConfig> config, std::shared_ptr<WorkerGuard> worker_guard,
                                     std::shared_ptr<storage::IStorageEngine> storage_engine,
                                     std::shared_ptr<NameIndex> name_index)
    : config_(std::move(config)),
      worker_guard_(std::move(worker_guard)),
      storage_engine_(std::move(storage_engine)),
      name_index_(std::move(name_index)) {
  if (!name_index_) {
    name_index_ = std::make_shared<NameIndex>();
  }
}

IndexOrchestrator::~IndexOrchestrator() {
  bg_stopping_.store(true);
  DoCancelScan();
  StopAndJoinBackgroundTask();
  StopAndJoinReclaim();
  IndexOrchestrator::DoStopLiveWatch();
}

std::mutex &IndexOrchestrator::LockBucketForPath(const std::string &file_path) const {
  std::string norm = file_path;
  for (char &c : norm) {
    if (c == '\\') c = '/';
  }
  const size_t bucket = std::hash<std::string>{}(norm) % path_locks_.size();
  return path_locks_[bucket];
}

void IndexOrchestrator::StopAndJoinReclaim() {
  stop_reclaim_.store(true);
  std::lock_guard<std::mutex> lock(reclaim_mutex_);
  if (space_reclaim_thread_.joinable()) {
    space_reclaim_thread_.join();
  }
}

bool IndexOrchestrator::DoInit() {
  if (!config_ || !worker_guard_ || !storage_engine_) {
    return false;
  }

  debouncer_ = std::make_shared<EventDebouncer>(config_->debounce_ms());
  debouncer_->SetCallback([this](const DebouncedEvent &ev) { HandleFileChange(ev); });

  // Cold-start recovery: load existing files metadata directly from database into NameIndex
  auto existing_files = storage_engine_->GetAllFilesMetadata();
  for (const auto &f : existing_files) {
    NameRecord rec;
    rec.doc_id = f.doc_id;
    rec.file_name = f.file_name;
    rec.file_path = f.file_path;
    rec.ext = f.ext;
    rec.file_size = f.file_size;
    rec.mtime = f.mtime;
    name_index_->Upsert(rec);
  }
  LOG_INFO("[IndexOrchestrator] Cold-started NameIndex with {} files from database.", existing_files.size());

  watch_engine_ = std::make_shared<WatchEngine>(config_, debouncer_);
  write_queue_ = std::make_shared<AsyncWriteQueue>();
  const auto profile = config_ ? config_->resource_profile() : common::ResourceProfile::kLowMemory;
  const auto tuning = common::GetResourceTuning(profile);
  if (storage_engine_) {
    storage_engine_->SetResourceProfile(profile);
  }
  write_queue_->Start(storage_engine_, tuning.queue_batch_size, std::chrono::seconds(10), tuning.queue_max_items,
                      tuning.queue_max_bytes);
  return true;
}

bool IndexOrchestrator::DoStartLiveWatch() {
  if (!watch_engine_) {
    LOG_ERROR("[IndexOrchestrator] WatchEngine is not initialized!");
    return false;
  }
  return watch_engine_->Start();
}

void IndexOrchestrator::DoStopLiveWatch() {
  if (watch_engine_) {
    watch_engine_->Stop();
  }
  if (debouncer_) {
    debouncer_->Flush();
  }
  DoFlushWriteQueue();
}

bool IndexOrchestrator::DoAddAndScanDirectory(const std::string &dir_path, bool /*recursive*/) {
  if (!watch_engine_ || !config_) {
    return false;
  }
  config_->AddIndexedDirectory(dir_path);
  if (!watch_engine_->AddWatch(dir_path)) {
    LOG_WARN("[IndexOrchestrator] Failed to add watch on directory '{}'", dir_path);
  }
  ScanDirectoryRecursive(dir_path);
  return true;
}

bool IndexOrchestrator::DoRemoveDirectoryIndex(const std::string &dir_path) {
  if (!watch_engine_ || !config_ || !storage_engine_) {
    return false;
  }

  // 1. Remove watch listener
  watch_engine_->RemoveWatch(dir_path);

  // 2. Remove from configuration
  config_->RemoveIndexedDirectory(dir_path);

  // 3. Deliver anything already debounced for this directory before purging it, then close the
  //    write barrier so no new batch transaction can start on the shared writer connection.
  if (debouncer_) {
    debouncer_->Flush();
  }
  if (write_queue_) {
    write_queue_->Pause();
  } else {
    DoFlushWriteQueue();
  }

  // 4. Cascade delete files in database
  std::vector<int64_t> deleted_doc_ids;
  int64_t deleted_count = storage_engine_->DeleteDirectory(dir_path, &deleted_doc_ids);
  LOG_INFO("[IndexOrchestrator] Removed directory '{}' from storage: {} files deleted", dir_path, deleted_count);

  // 5. Cascade delete from in-memory NameIndex
  if (name_index_) {
    name_index_->RemoveDirectoryPrefix(dir_path);
  }

  if (write_queue_) {
    write_queue_->Resume();
  }

  // 6. Purge failed files cache
  PurgeFailedFilesUnder(dir_path);

  // 7. Update statistics
  if (deleted_count > 0) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_files_deleted += deleted_count;
  }

  // 8. Trigger background vacuum if fragmented
  DoTriggerAsyncSpaceReclaim();

  return true;
}

void IndexOrchestrator::PurgeFailedFilesUnder(const std::string &dir_path) {
  std::lock_guard<std::mutex> lock(failed_files_mutex_);
  std::string normalized_dir = dir_path;
  if (!normalized_dir.empty() && normalized_dir.back() != '/' && normalized_dir.back() != '\\') {
    normalized_dir += '/';
  }

  for (auto it = failed_files_.begin(); it != failed_files_.end();) {
    if (it->first.rfind(normalized_dir, 0) == 0) {
      it = failed_files_.erase(it);
    } else {
      ++it;
    }
  }
}

void IndexOrchestrator::DoTriggerAsyncSpaceReclaim() {
  std::lock_guard<std::mutex> lock(reclaim_mutex_);
  if (is_reclaiming_space_.exchange(true)) {
    LOG_DEBUG("[IndexOrchestrator] Space reclaim is already running in background, skipping.");
    return;
  }

  if (space_reclaim_thread_.joinable()) {
    space_reclaim_thread_.join();
  }
  stop_reclaim_.store(false);

  space_reclaim_thread_ = std::thread([this]() {
    struct ReclaimFlagGuard {
      explicit ReclaimFlagGuard(std::atomic<bool> *flag) : flag_(flag) {}
      ~ReclaimFlagGuard() {
        if (flag_) flag_->store(false);
      }
      ReclaimFlagGuard(const ReclaimFlagGuard &) = delete;
      ReclaimFlagGuard &operator=(const ReclaimFlagGuard &) = delete;
      ReclaimFlagGuard(ReclaimFlagGuard &&) = delete;
      ReclaimFlagGuard &operator=(ReclaimFlagGuard &&) = delete;
      std::atomic<bool> *flag_;
    } flag_guard(&is_reclaiming_space_);

    LOG_INFO("[IndexOrchestrator] Starting asynchronous space reclamation check...");
    if (!storage_engine_ || stop_reclaim_.load()) {
      return;
    }

    storage::StorageFragmentation frag;
    if (!storage_engine_->GetFragmentation(&frag)) {
      LOG_WARN("[IndexOrchestrator] Failed to query database fragmentation status.");
      return;
    }

    LOG_INFO("[IndexOrchestrator] Storage status: total={:.2f} MB, free={:.2f} MB ({:.1f}%), free_pages={}",
             frag.total_bytes / (1024.0 * 1024.0), frag.free_bytes / (1024.0 * 1024.0), frag.frag_ratio * 100.0,
             frag.free_pages);

    if (frag.free_pages >= kMinFreePagesForVacuum || frag.frag_ratio >= kMinFragRatioForVacuum) {
      LOG_INFO("[IndexOrchestrator] Fragmentation exceeds threshold. Initiating paced incremental vacuum...");

      int64_t pages_left = frag.free_pages;
      while (pages_left > 0 && !stop_reclaim_.load()) {
        int pages_to_clean = static_cast<int>(std::min<int64_t>(pages_left, kVacuumChunkPages));
        if (!storage_engine_->IncrementalVacuum(pages_to_clean)) {
          LOG_WARN("[IndexOrchestrator] Incremental vacuum failed during paced cleanup.");
          break;
        }
        pages_left -= pages_to_clean;
        std::this_thread::sleep_for(kVacuumPacingInterval);
      }
      LOG_INFO("[IndexOrchestrator] Paced incremental vacuum completed successfully.");
    } else {
      LOG_DEBUG("[IndexOrchestrator] Fragmentation level is healthy, no vacuum required.");
    }
  });
}

size_t IndexOrchestrator::DoPerformInitialScan(ScanProgressCallback progress_cb) {
  std::lock_guard<std::mutex> scan_lock(scan_mutex_);
  return PerformScanInternal(std::move(progress_cb));
}

size_t IndexOrchestrator::PerformScanInternal(ScanProgressCallback progress_cb) {
  if (bg_stopping_.load()) {
    return 0;
  }
  cancel_scan_.store(false);
  std::vector<std::string> watched_dirs = config_ ? config_->GetIndexedDirectories() : std::vector<std::string>{};
  if (watched_dirs.empty() && watch_engine_) {
    watched_dirs = watch_engine_->GetWatchedDirectories();
  }

  std::vector<std::string> files_to_index;
  std::unordered_set<DirFingerprint, DirFingerprintHash> visited_dirs;
  for (const auto &dir : watched_dirs) {
    try {
      auto target_p = common::EncodingUtils::PathFromUtf8(dir);
      DirFingerprint root_fp;
      if (GetDirFingerprint(target_p, &root_fp)) {
        if (!visited_dirs.insert(root_fp).second) {
          LOG_INFO("[IndexOrchestrator] Watched directory '{}' is already scanned or a bind mount mirror, skipping.",
                   dir);
          continue;
        }
      }

      for (auto it = std::filesystem::recursive_directory_iterator(
               target_p, std::filesystem::directory_options::skip_permission_denied);
           it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (cancel_scan_.load()) {
          LOG_INFO("[IndexOrchestrator] Initial scan cancelled by user during candidate file discovery.");
          return 0;
        }
        if (it->is_directory()) {
          std::string sub_dir = common::EncodingUtils::PathToUtf8(it->path());
          if (config_ && config_->IsDirectoryExcluded(sub_dir)) {
            it.disable_recursion_pending();
            continue;
          }
          DirFingerprint fp;
          if (GetDirFingerprint(it->path(), &fp)) {
            if (!visited_dirs.insert(fp).second) {
              LOG_DEBUG("[IndexOrchestrator] Skipping duplicate/bind-mounted directory: '{}'", sub_dir);
              it.disable_recursion_pending();
              continue;
            }
          }
        }
        if (it->is_regular_file()) {
          std::string fpath = common::EncodingUtils::PathToUtf8(it->path());
          std::string fname = common::EncodingUtils::PathToUtf8(it->path().filename());
          std::string ext = ExtractExtension(fname);
          uint64_t fsize = 0;
          int64_t fmtime = 0;
          if (!common::EncodingUtils::GetFileStat(fpath, &fsize, &fmtime)) {
            try {
              fsize = it->file_size();
            } catch (const std::exception &e) {
              LOG_DEBUG("[IndexOrchestrator] Fallback file_size() failed for '{}': {}", fpath, e.what());
            }
          }

          if (config_ && config_->ShouldIndex(fpath, ext, fsize)) {
            files_to_index.push_back(fpath);
          }
        }
      }
    } catch (const std::exception &e) {
      LOG_WARN("[IndexOrchestrator] Failed to scan watched directory '{}': {}", dir, e.what());
    }
  }

  size_t total_files = files_to_index.size();
  LOG_INFO("[IndexOrchestrator] Found {} candidate files to inspect/index.", total_files);

  if (storage_engine_) {
    storage_engine_->SetBulkMode(true);
  }

  const size_t worker_concurrency = std::max<size_t>(1, config_ ? config_->worker_count() : 1);
  const size_t num_threads = std::min<size_t>(worker_concurrency, total_files > 0 ? total_files : 1);
  std::atomic<size_t> file_idx{0};
  std::atomic<size_t> indexed_count{0};
  std::atomic<size_t> processed_count{0};
  std::mutex progress_mutex;
  auto last_progress_report = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);

  std::vector<std::thread> workers;
  workers.reserve(num_threads);
  for (size_t t = 0; t < num_threads; ++t) {
    workers.emplace_back([&]() {
      while (!cancel_scan_.load(std::memory_order_acquire)) {
        const size_t idx = file_idx.fetch_add(1, std::memory_order_relaxed);
        if (idx >= total_files) {
          break;
        }
        const auto &file_path = files_to_index[idx];
        if (IndexSingleFile(file_path)) {
          indexed_count.fetch_add(1, std::memory_order_relaxed);
        }
        const size_t processed = processed_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (progress_cb) {
          std::lock_guard<std::mutex> lock(progress_mutex);
          const auto now = std::chrono::steady_clock::now();
          if (processed == total_files ||
              std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_report).count() >= 80) {
            last_progress_report = now;
            progress_cb(indexed_count.load(std::memory_order_relaxed), total_files, file_path);
          }
        }
      }
    });
  }

  for (auto &th : workers) {
    if (th.joinable()) {
      th.join();
    }
  }
  // Release scan path list immediately so malloc_trim(0) below can reclaim those heap pages.
  std::vector<std::string>().swap(files_to_index);

  if (cancel_scan_.load(std::memory_order_acquire)) {
    LOG_INFO("[IndexOrchestrator] Initial scan cancelled by user at {}/{} files.",
             processed_count.load(std::memory_order_relaxed), total_files);
  }

  const size_t final_indexed = indexed_count.load(std::memory_order_relaxed);
  if (progress_cb) {
    progress_cb(final_indexed, total_files, "");
  }

  DoFlushWriteQueue();
  if (storage_engine_) {
    storage_engine_->SetBulkMode(false);
  }
#if defined(__linux__) && defined(__GLIBC__)
  malloc_trim(0);
#endif
  LOG_INFO("[IndexOrchestrator] Initial scan completed. Indexed {}/{} files (concurrency={}).", final_indexed,
           total_files, num_threads);
  return final_indexed;
}

bool IndexOrchestrator::DoRebuildIndex(ScanProgressCallback progress_cb) {
  std::lock_guard<std::mutex> scan_lock(scan_mutex_);
  if (bg_stopping_.load()) {
    return false;
  }
  LOG_INFO("[IndexOrchestrator] Starting complete index rebuild...");
  cancel_scan_.store(false);

  // Stop background space reclamation before clearing storage so vacuum and ClearAll never collide.
  StopAndJoinReclaim();

  // 1. Quiesce live monitoring first: event callbacks run on the debouncer thread and may
  //    mutate NameIndex / the writer connection while we clear persistent state. Pending
  //    debounced events are dropped because the full rescan below re-discovers every file.
  const bool was_watching = watch_engine_ && watch_engine_->is_watching();
  if (watch_engine_) {
    watch_engine_->Stop();
  }
  if (debouncer_) {
    debouncer_->ClearPending();
  }

  // 2. Close the write barrier and drain any in-flight batch transaction. ClearAll() issues
  //    BEGIN on the shared writer connection, which fails if a batch transaction is open.
  if (write_queue_) {
    write_queue_->Pause();
  } else {
    DoFlushWriteQueue();
  }

  // 3. Clear persistent storage first; only drop the in-memory mirror once the DB was cleared
  //    successfully, otherwise a failure would leave NameIndex empty while the DB still has data.
  bool cleared = true;
  if (storage_engine_) {
    cleared = storage_engine_->ClearAll();
    if (!cleared) {
      LOG_ERROR("[IndexOrchestrator] Failed to clear persistent storage during rebuild!");
    } else {
      LOG_INFO("[IndexOrchestrator] Cleared persistent storage and FTS5 index.");
    }
  }
  if (cleared && name_index_) {
    // In-place clear preserves shared_ptr identity across CQRS readers.
    name_index_->Clear();
  }
  if (write_queue_) {
    write_queue_->Resume();
  }
  if (!cleared) {
    RestartLiveWatch(was_watching);
    return false;
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

  // 5. Perform clean initial scan
  size_t indexed = PerformScanInternal(std::move(progress_cb));

  // 6. Restore live monitoring
  RestartLiveWatch(was_watching);

  if (cancel_scan_.load()) {
    LOG_INFO("[IndexOrchestrator] Rebuild cancelled after indexing {} files.", indexed);
    return false;
  }
  LOG_INFO("[IndexOrchestrator] Rebuild completed successfully. Total files indexed: {}", indexed);
  return true;
}

void IndexOrchestrator::RestartLiveWatch(bool was_watching) {
  if (!was_watching || !watch_engine_ || !config_) {
    return;
  }
  // WatchEngine::Stop() removes the underlying watches, so re-register the configured
  // directories before starting the watcher again.
  const std::vector<std::string> dirs = config_->GetIndexedDirectories();
  for (const auto &dir : dirs) {
    if (!watch_engine_->AddWatch(dir)) {
      LOG_WARN("[IndexOrchestrator] Failed to restore watch on '{}' after rebuild", dir);
    }
  }
  watch_engine_->Start();
}

void IndexOrchestrator::ScanDirectoryRecursive(const std::string &dir_path) {
  try {
    auto target_p = common::EncodingUtils::PathFromUtf8(dir_path);
    for (auto it = std::filesystem::recursive_directory_iterator(
             target_p, std::filesystem::directory_options::skip_permission_denied);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (cancel_scan_.load()) break;

      if (it->is_directory()) {
        std::string sub_dir = common::EncodingUtils::PathToUtf8(it->path());
        if (config_ && config_->IsDirectoryExcluded(sub_dir)) {
          it.disable_recursion_pending();
          continue;
        }
      }
      if (it->is_regular_file()) {
        std::string fpath = common::EncodingUtils::PathToUtf8(it->path());
        IndexSingleFile(fpath);
      }
    }
  } catch (const std::exception &e) {
    LOG_WARN("[IndexOrchestrator] Error walking directory '{}': {}", dir_path, e.what());
  }
}

bool IndexOrchestrator::IndexSingleFile(const std::string &file_path) {
  std::lock_guard<std::mutex> path_lock(LockBucketForPath(file_path));
  return IndexSingleFileLocked(file_path);
}

bool IndexOrchestrator::IndexSingleFileLocked(const std::string &file_path) {
  auto start_time = std::chrono::high_resolution_clock::now();

  uint64_t file_size = 0;
  int64_t file_mtime = 0;
  if (!common::EncodingUtils::GetFileStat(file_path, &file_size, &file_mtime)) {
    LOG_WARN("[IndexOrchestrator] Failed to stat file: '{}'", file_path);
    return false;
  }

  std::string filename = ExtractFileName(file_path);
  std::string ext = ExtractExtension(filename);

  if (!config_ || !config_->ShouldIndex(file_path, ext, file_size)) {
    return false;
  }

  // Check if previously marked as failed/corrupt and file has not changed
  {
    std::lock_guard<std::mutex> lock(failed_files_mutex_);
    auto it = failed_files_.find(file_path);
    if (it != failed_files_.end() && it->second.file_size == file_size && it->second.mtime == file_mtime) {
      return false;
    }
  }

  int64_t doc_id = GenerateDocId(file_path);

  // Fast incremental check
  uint64_t db_size = 0;
  int64_t db_mtime = 0;
  const bool meta_found = (name_index_ && name_index_->GetMetadata(doc_id, &db_size, &db_mtime)) ||
                          (storage_engine_ && storage_engine_->GetFileMetadata(doc_id, &db_size, &db_mtime));

  if (meta_found && db_size == file_size && db_mtime == file_mtime) {
    if (name_index_) {
      NameRecord rec{doc_id, filename, file_path, ext, file_size, file_mtime};
      name_index_->Upsert(rec);
    }
    return true;
  }

  // Pre-check
  auto precheck = DocPrechecker::Check(file_path, ext, file_size);
  if (precheck.status != PrecheckStatus::kValid) {
    LOG_WARN("[IndexOrchestrator] Precheck skipped body for '{}' (ext: '{}') | reason: {}", file_path, ext,
             precheck.reason);
    PersistIndexedFile(doc_id, file_path, filename, ext, file_size, file_mtime, "");

    std::lock_guard<std::mutex> lock(failed_files_mutex_);
    failed_files_[file_path] = {file_size, file_mtime, precheck.reason};
    return true;
  }

  // Dispatch to worker sandbox pool
  ParseTask task{doc_id, file_path, ext, 1, precheck.recommended_timeout_ms};
  ParseTaskResult res;

  auto parse_start = std::chrono::high_resolution_clock::now();
  bool ok = worker_guard_->ExecuteTask(task, &res);
  auto parse_end = std::chrono::high_resolution_clock::now();
  double parse_cost_ms = std::chrono::duration<double, std::milli>(parse_end - parse_start).count();

  if (!ok || res.status != TaskStatus::kSuccess) {
    if (res.slot_id > 0) {
      worker_guard_->ReleaseSlot(res.slot_id);
    }
    if (!res.spill_file_path.empty()) {
      std::error_code ec;
      std::filesystem::remove(res.spill_file_path, ec);
    }
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_parse_failures++;
    LOG_ERROR(
        "[IndexOrchestrator] FAILED to parse '{}' (ext: '{}', size: {} B) in {:.2f} ms | "
        "status: {} | err: {}",
        file_path, ext, file_size, parse_cost_ms, static_cast<int>(res.status), res.error_message);

    // Index filename into NameIndex and StorageEngine so it remains searchable
    PersistIndexedFile(doc_id, file_path, filename, ext, file_size, file_mtime, "");

    std::lock_guard<std::mutex> flock(failed_files_mutex_);
    failed_files_[file_path] = {file_size, file_mtime, res.error_message};
    return false;
  }

  // Read extracted plain text from shared memory slot or spill file
  std::string parsed_text;
  size_t extracted_bytes = 0;

  struct SpillFileGuard {
    explicit SpillFileGuard(std::string p) : path(std::move(p)) {}
    ~SpillFileGuard() {
      if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
      }
    }
    SpillFileGuard(const SpillFileGuard &) = delete;
    SpillFileGuard &operator=(const SpillFileGuard &) = delete;
    SpillFileGuard(SpillFileGuard &&) = delete;
    SpillFileGuard &operator=(SpillFileGuard &&) = delete;
    std::string path;
  } spill_guard{res.spill_file_path};

  if (!res.spill_file_path.empty()) {
    std::error_code ec;
    auto ifs = common::EncodingUtils::OpenIfstream(res.spill_file_path);
    if (ifs.is_open()) {
      parsed_text.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
      ifs.close();
      extracted_bytes = parsed_text.size();
    }
    std::filesystem::remove(res.spill_file_path, ec);
    spill_guard.path.clear();
  } else {
    const uint8_t *extracted_data = nullptr;
    bool read_ok = worker_guard_->shm_pool()->ReadSlot(res.slot_id, &extracted_data, &extracted_bytes);
    if (read_ok && extracted_data && extracted_bytes > 0) {
      parsed_text.assign(reinterpret_cast<const char *>(extracted_data), extracted_bytes);
    }
  }
  worker_guard_->ReleaseSlot(res.slot_id);

  PersistIndexedFile(doc_id, file_path, filename, ext, file_size, file_mtime, std::move(parsed_text));

  auto total_end = std::chrono::high_resolution_clock::now();
  double total_cost_ms = std::chrono::duration<double, std::milli>(total_end - start_time).count();

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_files_indexed++;
    stats_.total_bytes_indexed += extracted_bytes;
  }

  LOG_INFO(
      "[IndexOrchestrator] SUCCESS parsed & enqueued '{}' (ext: '{}', size: {} B, extracted: {} B) | "
      "parse: {:.2f} ms, total: {:.2f} ms",
      file_path, ext, file_size, extracted_bytes, parse_cost_ms, total_cost_ms);

  return true;
}

void IndexOrchestrator::PersistIndexedFile(int64_t doc_id, const std::string &file_path, const std::string &filename,
                                           const std::string &ext, uint64_t file_size, int64_t file_mtime,
                                           std::string content) {
  if (name_index_) {
    NameRecord rec{doc_id, filename, file_path, ext, file_size, file_mtime};
    name_index_->Upsert(rec);
  }

  if (write_queue_ && write_queue_->is_running()) {
    WriteOp op{WriteOpType::kIndex, doc_id, file_path, filename, ext, file_size, file_mtime, 1, std::move(content)};
    if (write_queue_->Enqueue(&op)) {
      return;
    }
    LOG_WARN("[IndexOrchestrator] Write queue rejected '{}' during shutdown, falling back to synchronous write",
             file_path);
    if (storage_engine_) {
      storage_engine_->IndexDocument(op.doc_id, op.file_path, op.file_name, op.ext, op.file_size, op.mtime,
                                     op.parse_level, op.content);
    }
    return;
  }

  if (storage_engine_) {
    storage_engine_->IndexDocument(doc_id, file_path, filename, ext, file_size, file_mtime, 1, content);
  }
}

bool IndexOrchestrator::DoHandleFileChange(const DebouncedEvent &event) {
  std::lock_guard<std::mutex> path_lock(LockBucketForPath(event.file_path));
  int64_t doc_id = GenerateDocId(event.file_path);

  if (event.action == FileChangeAction::kDelete) {
    LOG_INFO("[IndexOrchestrator] File delete event: '{}' | removing from index", event.file_path);
    if (name_index_) {
      name_index_->Remove(doc_id);
    }
    if (write_queue_ && write_queue_->is_running()) {
      WriteOp op;
      op.type = WriteOpType::kDelete;
      op.doc_id = doc_id;
      if (!write_queue_->Enqueue(&op) && storage_engine_) {
        storage_engine_->DeleteDocument(doc_id);
      }
    } else if (storage_engine_) {
      storage_engine_->DeleteDocument(doc_id);
    }

    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_files_deleted++;
    return true;
  }

  if (event.action == FileChangeAction::kAdd) {
    LOG_INFO("[IndexOrchestrator] File added event: '{}' | triggering index", event.file_path);
    return IndexSingleFileLocked(event.file_path);
  }

  if (event.action == FileChangeAction::kModified) {
    LOG_INFO("[IndexOrchestrator] File modified event: '{}' | triggering re-index", event.file_path);
    return IndexSingleFileLocked(event.file_path);
  }

  return true;
}

void IndexOrchestrator::DoFlushWriteQueue() {
  if (write_queue_) {
    write_queue_->Flush();
  }
}

PipelineStats IndexOrchestrator::DoGetStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

void IndexOrchestrator::DoCancelScan() {
  {
    std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
    pending_task_ = PendingTask::kNone;
  }
  cancel_scan_.store(true);
}

void IndexOrchestrator::DoApplyRuntimeConfig() {
  if (!config_) {
    return;
  }
  common::Logger::SetLevel(config_->log_level());
  common::Logger::SetConsoleEnabled(config_->log_console());
  common::Logger::SetSyncMode(config_->log_sync());
  if (worker_guard_) {
    worker_guard_->SetLoggingOptions(config_->log_level(), config_->log_console(), config_->log_sync(),
                                     config_->doctotext_logging(), config_->doctotext_log_path(),
                                     config_->enable_embedded_image_ocr());
  }
  const auto profile = config_->resource_profile();
  const auto tuning = common::GetResourceTuning(profile);
  if (storage_engine_) {
    storage_engine_->SetResourceProfile(profile);
  }
  if (write_queue_) {
    write_queue_->UpdateLimits(tuning.queue_batch_size, tuning.queue_max_items, tuning.queue_max_bytes);
  }
}

void IndexOrchestrator::DoSyncWatchedDirectories() {
  if (!watch_engine_ || !config_) {
    return;
  }
  const auto configured_dirs = config_->GetIndexedDirectories();
  const auto watched_dirs = watch_engine_->GetWatchedDirectories();
  const std::unordered_set<std::string> active_set(configured_dirs.begin(), configured_dirs.end());

  for (const auto &watched : watched_dirs) {
    if (cancel_scan_.load() || bg_stopping_.load()) {
      break;
    }
    if (active_set.find(watched) == active_set.end()) {
      DoRemoveDirectoryIndex(watched);
    }
  }
  for (const auto &dir : configured_dirs) {
    if (cancel_scan_.load() || bg_stopping_.load()) {
      break;
    }
    watch_engine_->AddWatch(dir);
  }
}

void IndexOrchestrator::DoClearBackgroundCallbacks() {
  std::lock_guard<std::mutex> cb_lock(bg_callback_mutex_);
  bg_callbacks_ = BackgroundIndexCallbacks{};
}

bool IndexOrchestrator::DoIsIndexing() const {
  std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
  return bg_task_running_ || pending_task_ != PendingTask::kNone;
}

size_t IndexOrchestrator::DoIndexedFileCount() const {
  return name_index_ ? name_index_->size() : 0;
}

void IndexOrchestrator::StopAndJoinBackgroundTask() {
  std::lock_guard<std::mutex> join_lock(bg_join_mutex_);
  bg_stopping_.store(true);
  {
    std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
    pending_task_ = PendingTask::kNone;
  }
  cancel_scan_.store(true);
  if (bg_task_thread_.joinable()) {
    bg_task_thread_.join();
  }
  std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
  bg_task_running_ = false;
  pending_task_ = PendingTask::kNone;
}

void IndexOrchestrator::DoStartBackgroundTask(bool rebuild, bool force_incremental,
                                              BackgroundIndexCallbacks callbacks) {
  std::lock_guard<std::mutex> join_lock(bg_join_mutex_);
  if (bg_stopping_.load()) {
    return;
  }

  {
    std::lock_guard<std::mutex> cb_lock(bg_callback_mutex_);
    bg_callbacks_ = std::move(callbacks);
  }

  const PendingTask req_pending = rebuild ? PendingTask::kRebuild : PendingTask::kIncremental;
  {
    std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
    if (bg_task_running_) {
      // Preserve higher-priority follow-up task so a rebuild is never downgraded into an incremental sync.
      if (static_cast<int>(req_pending) > static_cast<int>(pending_task_)) {
        pending_task_ = req_pending;
      }
      cancel_scan_.store(true);
      return;
    }
  }

  // Previous task already cleared bg_task_running_ = false and is exiting; join safely under bg_join_mutex_.
  if (bg_task_thread_.joinable()) {
    bg_task_thread_.join();
  }

  const bool is_cold = !rebuild && !force_incremental && (!name_index_ || name_index_->size() == 0);
  const IndexTaskKind initial_kind =
      rebuild ? IndexTaskKind::kRebuild : (is_cold ? IndexTaskKind::kInitial : IndexTaskKind::kIncremental);
  uint64_t initial_generation = 0;

  {
    std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
    if (bg_stopping_.load()) {
      return;
    }
    bg_task_running_ = true;
    pending_task_ = PendingTask::kNone;
    initial_generation = bg_generation_.fetch_add(1) + 1;
  }

  bg_task_thread_ = std::thread(&IndexOrchestrator::BackgroundTaskLoop, this, initial_kind, initial_generation);
}

void IndexOrchestrator::BackgroundTaskLoop(IndexTaskKind initial_kind, uint64_t initial_generation) {
  IndexTaskKind kind = initial_kind;
  uint64_t generation = initial_generation;

  while (true) {
    {
      std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
      if (bg_stopping_.load()) {
        bg_task_running_ = false;
        pending_task_ = PendingTask::kNone;
        break;
      }
      cancel_scan_.store(false);
    }

    IndexTaskStartedCallback on_started;
    {
      std::lock_guard<std::mutex> cb_lock(bg_callback_mutex_);
      on_started = bg_callbacks_.on_started;
    }
    if (on_started) {
      on_started(kind, generation);
    }

    const auto t0 = std::chrono::steady_clock::now();
    bool ok = true;
    size_t count = 0;

    auto progress_forwarder = [this, kind, generation](size_t indexed, size_t total, const std::string &path) {
      IndexTaskProgressCallback on_progress;
      {
        std::lock_guard<std::mutex> cb_lock(bg_callback_mutex_);
        on_progress = bg_callbacks_.on_progress;
      }
      if (on_progress) {
        on_progress(kind, generation, indexed, total, path);
      }
    };

    if (kind == IndexTaskKind::kRebuild) {
      ok = DoRebuildIndex(std::move(progress_forwarder));
      count = ok ? DoIndexedFileCount() : 0;
    } else {
      DoSyncWatchedDirectories();
      if (!cancel_scan_.load() && !bg_stopping_.load()) {
        count = DoPerformInitialScan(std::move(progress_forwarder));
      }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
    const bool cancelled = cancel_scan_.load();

    PendingTask next_pending = PendingTask::kNone;
    uint64_t next_generation = 0;
    {
      std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
      if (bg_stopping_.load()) {
        pending_task_ = PendingTask::kNone;
      }
      next_pending = pending_task_;
      pending_task_ = PendingTask::kNone;
      if (next_pending == PendingTask::kNone) {
        bg_task_running_ = false;
      } else {
        next_generation = bg_generation_.fetch_add(1) + 1;
      }
    }

    IndexTaskFinishedCallback on_finished;
    {
      std::lock_guard<std::mutex> cb_lock(bg_callback_mutex_);
      on_finished = bg_callbacks_.on_finished;
    }
    if (on_finished) {
      IndexTaskResult result;
      result.kind = kind;
      result.generation = generation;
      result.total_indexed = count;
      result.success = ok;
      result.cancelled = cancelled;
      result.elapsed_sec = elapsed_sec;
      result.has_pending_followup = (next_pending != PendingTask::kNone);
      on_finished(result);
    }

    if (next_pending == PendingTask::kNone) {
      break;
    }
    kind = (next_pending == PendingTask::kRebuild) ? IndexTaskKind::kRebuild : IndexTaskKind::kIncremental;
    generation = next_generation;
  }
}

}  // namespace service
}  // namespace soeasy
