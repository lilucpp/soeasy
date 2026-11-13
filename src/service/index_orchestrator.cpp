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
#include "service/doc_prechecker.h"

namespace soeasy {
namespace service {

namespace {

struct DirFingerprint {
  uint64_t dev{0};
  uint64_t ino{0};
  bool operator==(const DirFingerprint &other) const {
    return dev == other.dev && ino == other.ino;
  }
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
  HANDLE h_file = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
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
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
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
                                     std::shared_ptr<storage::StorageEngine> storage_engine,
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
  cancel_scan_.store(true);
  StopLiveWatch();
  if (space_reclaim_thread_.joinable()) {
    space_reclaim_thread_.join();
  }
}

bool IndexOrchestrator::Init() {
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
  write_queue_->Start(storage_engine_, 100, std::chrono::seconds(10), 500);
  return true;
}

bool IndexOrchestrator::StartLiveWatch() {
  if (!watch_engine_) {
    LOG_ERROR("[IndexOrchestrator] WatchEngine is not initialized!");
    return false;
  }
  return watch_engine_->Start();
}

void IndexOrchestrator::StopLiveWatch() {
  if (watch_engine_) {
    watch_engine_->Stop();
  }
  if (debouncer_) {
    debouncer_->Flush();
  }
  FlushWriteQueue();
}

bool IndexOrchestrator::AddAndScanDirectory(const std::string &dir_path, bool recursive) {
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

bool IndexOrchestrator::RemoveDirectoryIndex(const std::string &dir_path) {
  if (!watch_engine_ || !config_ || !storage_engine_) {
    return false;
  }

  // 1. Remove watch listener
  watch_engine_->RemoveWatch(dir_path);

  // 2. Remove from configuration
  config_->RemoveIndexedDirectory(dir_path);

  // 3. Purge write queue to avoid race conditions
  FlushWriteQueue();

  // 4. Cascade delete files in database
  std::vector<int64_t> deleted_doc_ids;
  int64_t deleted_count = storage_engine_->DeleteDirectory(dir_path, &deleted_doc_ids);
  LOG_INFO("[IndexOrchestrator] Removed directory '{}' from storage: {} files deleted", dir_path, deleted_count);

  // 5. Cascade delete from in-memory NameIndex
  if (name_index_) {
    name_index_->RemoveDirectoryPrefix(dir_path);
  }

  // 6. Purge failed files cache
  PurgeFailedFilesUnder(dir_path);

  // 7. Update statistics
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_files_deleted += deleted_count;
  }

  // 8. Trigger background vacuum if fragmented
  TriggerAsyncSpaceReclaim();

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

void IndexOrchestrator::TriggerAsyncSpaceReclaim() {
  if (is_reclaiming_space_.exchange(true)) {
    LOG_DEBUG("[IndexOrchestrator] Space reclaim is already running in background, skipping.");
    return;
  }

  if (space_reclaim_thread_.joinable()) {
    space_reclaim_thread_.join();
  }

  space_reclaim_thread_ = std::thread([this]() {
    LOG_INFO("[IndexOrchestrator] Starting asynchronous space reclamation check...");
    if (!storage_engine_) {
      is_reclaiming_space_.store(false);
      return;
    }

    storage::StorageFragmentation frag;
    if (!storage_engine_->GetFragmentation(&frag)) {
      LOG_WARN("[IndexOrchestrator] Failed to query database fragmentation status.");
      is_reclaiming_space_.store(false);
      return;
    }

    LOG_INFO("[IndexOrchestrator] Storage status: total={:.2f} MB, free={:.2f} MB ({:.1f}%), free_pages={}",
             frag.total_bytes / (1024.0 * 1024.0), frag.free_bytes / (1024.0 * 1024.0),
             frag.frag_ratio * 100.0, frag.free_pages);

    if (frag.free_pages >= kMinFreePagesForVacuum || frag.frag_ratio >= kMinFragRatioForVacuum) {
      LOG_INFO("[IndexOrchestrator] Fragmentation exceeds threshold. Initiating paced incremental vacuum...");

      int64_t pages_left = frag.free_pages;
      while (pages_left > 0 && !cancel_scan_.load()) {
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

    is_reclaiming_space_.store(false);
  });
}

size_t IndexOrchestrator::PerformInitialScan(ScanProgressCallback progress_cb) {
  cancel_scan_.store(false);
  auto watched_dirs = config_->GetIndexedDirectories();
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
          LOG_INFO("[IndexOrchestrator] Watched directory '{}' is already scanned or a bind mount mirror, skipping.", dir);
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
            } catch (...) {
            }
          }

          if (config_->ShouldIndex(fpath, ext, fsize)) {
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

  if (cancel_scan_.load(std::memory_order_acquire)) {
    LOG_INFO("[IndexOrchestrator] Initial scan cancelled by user at {}/{} files.",
             processed_count.load(std::memory_order_relaxed), total_files);
  }

  const size_t final_indexed = indexed_count.load(std::memory_order_relaxed);
  if (progress_cb) {
    progress_cb(final_indexed, total_files, "");
  }

  FlushWriteQueue();
  if (storage_engine_) {
    storage_engine_->SetBulkMode(false);
  }
  LOG_INFO("[IndexOrchestrator] Initial scan completed. Indexed {}/{} files (concurrency={}).", final_indexed,
           total_files, num_threads);
  return final_indexed;
}

bool IndexOrchestrator::RebuildIndex(ScanProgressCallback progress_cb) {
  LOG_INFO("[IndexOrchestrator] Starting complete index rebuild...");
  cancel_scan_.store(false);

  // 1. Drain write queue
  FlushWriteQueue();

  // 2. Clear in-memory NameIndex
  if (name_index_) {
    name_index_ = std::make_shared<NameIndex>();
  }

  // 3. Clear StorageEngine tables
  if (storage_engine_) {
    if (!storage_engine_->ClearAll()) {
      LOG_ERROR("[IndexOrchestrator] Failed to clear persistent storage during rebuild!");
      return false;
    }
    LOG_INFO("[IndexOrchestrator] Cleared persistent storage and FTS5 index.");
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
  size_t indexed = PerformInitialScan(progress_cb);
  LOG_INFO("[IndexOrchestrator] Rebuild completed successfully. Total files indexed: {}", indexed);
  return true;
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
  auto start_time = std::chrono::high_resolution_clock::now();

  uint64_t file_size = 0;
  int64_t file_mtime = 0;
  if (!common::EncodingUtils::GetFileStat(file_path, &file_size, &file_mtime)) {
    LOG_WARN("[IndexOrchestrator] Failed to stat file: '{}'", file_path);
    return false;
  }

  std::string filename = ExtractFileName(file_path);
  std::string ext = ExtractExtension(filename);

  if (!config_->ShouldIndex(file_path, ext, file_size)) {
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
  bool meta_found = false;
  if (name_index_ && name_index_->GetMetadata(doc_id, &db_size, &db_mtime)) {
    meta_found = true;
  } else if (storage_engine_ && storage_engine_->GetFileMetadata(doc_id, &db_size, &db_mtime)) {
    meta_found = true;
  }

  if (meta_found && db_size == file_size && db_mtime == file_mtime) {
    NameRecord rec;
    rec.doc_id = doc_id;
    rec.file_name = filename;
    rec.file_path = file_path;
    rec.ext = ext;
    rec.file_size = file_size;
    rec.mtime = file_mtime;
    name_index_->Upsert(rec);
    return true;
  }

  // Pre-check
  auto precheck = DocPrechecker::Check(file_path, ext, file_size);
  if (precheck.status != PrecheckStatus::kValid) {
    LOG_WARN("[IndexOrchestrator] Precheck skipped body for '{}' (ext: '{}') | reason: {}", file_path, ext,
             precheck.reason);
    NameRecord rec;
    rec.doc_id = doc_id;
    rec.file_name = filename;
    rec.file_path = file_path;
    rec.ext = ext;
    rec.file_size = file_size;
    rec.mtime = file_mtime;
    name_index_->Upsert(rec);

    if (write_queue_ && write_queue_->is_running()) {
      WriteOp op{WriteOpType::kIndex, doc_id, file_path, filename, ext, file_size, file_mtime, 1, ""};
      write_queue_->Enqueue(std::move(op));
    } else if (storage_engine_) {
      storage_engine_->IndexDocument(doc_id, file_path, filename, ext, file_size, file_mtime, 1, "");
    }

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
    NameRecord rec;
    rec.doc_id = doc_id;
    rec.file_name = filename;
    rec.file_path = file_path;
    rec.ext = ext;
    rec.file_size = file_size;
    rec.mtime = file_mtime;
    name_index_->Upsert(rec);

    if (write_queue_ && write_queue_->is_running()) {
      WriteOp op{WriteOpType::kIndex, doc_id, file_path, filename, ext, file_size, file_mtime, 1, ""};
      write_queue_->Enqueue(std::move(op));
    } else if (storage_engine_) {
      storage_engine_->IndexDocument(doc_id, file_path, filename, ext, file_size, file_mtime, 1, "");
    }

    std::lock_guard<std::mutex> flock(failed_files_mutex_);
    failed_files_[file_path] = {file_size, file_mtime, res.error_message};
    return false;
  }

  // Read extracted plain text from shared memory slot or spill file
  std::string parsed_text;
  size_t extracted_bytes = 0;

  struct SpillFileGuard {
    std::string path;
    ~SpillFileGuard() {
      if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
      }
    }
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

  // Commit to NameIndex
  NameRecord rec;
  rec.doc_id = doc_id;
  rec.file_name = filename;
  rec.file_path = file_path;
  rec.ext = ext;
  rec.file_size = file_size;
  rec.mtime = file_mtime;
  name_index_->Upsert(rec);

  // Commit to StorageEngine via AsyncWriteQueue
  if (write_queue_ && write_queue_->is_running()) {
    WriteOp op{WriteOpType::kIndex, doc_id, file_path, filename, ext, file_size, file_mtime, 1, std::move(parsed_text)};
    write_queue_->Enqueue(std::move(op));
  } else if (storage_engine_) {
    storage_engine_->IndexDocument(doc_id, file_path, filename, ext, file_size, file_mtime, 1, parsed_text);
  }

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

bool IndexOrchestrator::HandleFileChange(const DebouncedEvent &event) {
  int64_t doc_id = GenerateDocId(event.file_path);

  if (event.action == FileChangeAction::kDelete) {
    LOG_INFO("[IndexOrchestrator] File delete event: '{}' | removing from index", event.file_path);
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
    LOG_INFO("[IndexOrchestrator] File added event: '{}' | triggering index", event.file_path);
    return IndexSingleFile(event.file_path);
  }

  if (event.action == FileChangeAction::kModified) {
    LOG_INFO("[IndexOrchestrator] File modified event: '{}' | triggering re-index", event.file_path);
    return IndexSingleFile(event.file_path);
  }

  return true;
}

void IndexOrchestrator::FlushWriteQueue() {
  if (write_queue_) {
    write_queue_->Flush();
  }
}

PipelineStats IndexOrchestrator::GetStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

}  // namespace service
}  // namespace soeasy
