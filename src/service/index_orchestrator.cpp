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

struct ScopedPhaseResetGuard {
  ScopedPhaseResetGuard(std::atomic<soeasy::service::OrchestratorPhase> *phase,
                        soeasy::service::OrchestratorPhase enter_phase, bool active)
      : phase_(phase), active_(active) {
    if (active_ && phase_) {
      phase_->store(enter_phase, std::memory_order_release);
    }
  }
  ~ScopedPhaseResetGuard() {
    if (active_ && phase_) {
      phase_->store(soeasy::service::OrchestratorPhase::kIdle, std::memory_order_release);
    }
  }
  ScopedPhaseResetGuard(const ScopedPhaseResetGuard &) = delete;
  ScopedPhaseResetGuard &operator=(const ScopedPhaseResetGuard &) = delete;

  std::atomic<soeasy::service::OrchestratorPhase> *phase_;
  bool active_;
};

std::string NormalizeDirPrefixSlash(const std::string &dir_path) {
  const std::string norm = soeasy::service::NormalizeDirKey(dir_path);
  return (norm == "/") ? norm : (norm + "/");
}

bool IsEmbeddedOcrCandidateExt(const std::string &ext) {
  static const std::unordered_set<std::string> kOcrDocExts = {
      "pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt", "ods", "odp", "odg", "wps", "et", "dps", "rtf"};
  return kOcrDocExts.find(ext) != kOcrDocExts.end();
}

bool PathStartsWithNormalizedDir(const std::string &file_path, const std::string &dir_prefix_slash) {
  std::string norm_file = file_path;
  std::replace(norm_file.begin(), norm_file.end(), '\\', '/');
  if (dir_prefix_slash == "/") {
    return !norm_file.empty() && norm_file[0] == '/';
  }
  if (norm_file.size() >= dir_prefix_slash.size() &&
      norm_file.compare(0, dir_prefix_slash.size(), dir_prefix_slash) == 0) {
    return true;
  }
  const std::string exact_dir = dir_prefix_slash.substr(0, dir_prefix_slash.size() - 1);
  return norm_file == exact_dir;
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
  watch_engine_->SetPathKnownCallback(
      [this](const std::string &p) { return name_index_ ? name_index_->ContainsPathOrPrefix(p) : false; });
  std::string incomplete_flag;
  if (storage_engine_->GetMetaValue("rebuild_incomplete", &incomplete_flag) && incomplete_flag == "1") {
    rebuild_incomplete_.store(true, std::memory_order_release);
    LOG_WARN("[IndexOrchestrator] Detected incomplete rebuild flag from previous session.");
  }
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
  if (config_) {
    for (const auto &dir : config_->GetIndexedDirectories()) {
      watch_engine_->AddWatch(dir);
    }
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

void IndexOrchestrator::SetRebuildIncomplete(bool incomplete) {
  rebuild_incomplete_.store(incomplete, std::memory_order_release);
  if (storage_engine_) {
    storage_engine_->SetMetaValue("rebuild_incomplete", incomplete ? "1" : "0");
  }
}

void IndexOrchestrator::EnableWatchBuffering(bool clear_existing) {
  std::lock_guard<std::mutex> dispatch_lock(live_dispatch_mutex_);
  std::lock_guard<std::mutex> buf_lock(buffered_events_mutex_);
  if (clear_existing) {
    buffered_watch_events_.clear();
  }
  buffer_watch_events_.store(true, std::memory_order_release);
}

void IndexOrchestrator::RecordBufferedEventUnlocked(const DebouncedEvent &event) {
  auto merge_single = [&](const std::string &path, FileChangeAction action, const std::string &old_path) {
    auto it = buffered_watch_events_.find(path);
    if (it == buffered_watch_events_.end()) {
      buffered_watch_events_[path] = DebouncedEvent{path, old_path, action, event.last_event_time};
      return;
    }
    it->second.last_event_time = event.last_event_time;
    const FileChangeAction prev = it->second.action;
    if (action == FileChangeAction::kDelete) {
      // Keep explicit delete so reconciliation removes records the scan may have visited before deletion.
      it->second.action = FileChangeAction::kDelete;
    } else if (prev == FileChangeAction::kDelete || action == FileChangeAction::kModified ||
               prev == FileChangeAction::kModified) {
      it->second.action = FileChangeAction::kModified;
    } else {
      it->second.action = action;
    }
  };

  if (event.action == FileChangeAction::kMoved && !event.old_path.empty()) {
    merge_single(event.old_path, FileChangeAction::kDelete, "");
    merge_single(event.file_path, FileChangeAction::kAdd, event.old_path);
    return;
  }
  merge_single(event.file_path, event.action, event.old_path);
}

void IndexOrchestrator::ReconcileAndDisableWatchBuffering() {
  phase_.store(OrchestratorPhase::kReconciling, std::memory_order_release);
  while (true) {
    std::vector<DebouncedEvent> batch;
    {
      std::lock_guard<std::mutex> buf_lock(buffered_events_mutex_);
      if (!buffered_watch_events_.empty() && !bg_stopping_.load(std::memory_order_acquire)) {
        batch.reserve(buffered_watch_events_.size());
        for (auto &kv : buffered_watch_events_) {
          batch.push_back(std::move(kv.second));
        }
        buffered_watch_events_.clear();
      }
    }

    if (batch.empty()) {
      std::lock_guard<std::mutex> dispatch_lock(live_dispatch_mutex_);
      std::lock_guard<std::mutex> buf_lock(buffered_events_mutex_);
      if (buffered_watch_events_.empty() || bg_stopping_.load(std::memory_order_acquire)) {
        buffered_watch_events_.clear();
        buffer_watch_events_.store(false, std::memory_order_release);
        return;
      }
      batch.reserve(buffered_watch_events_.size());
      for (auto &kv : buffered_watch_events_) {
        batch.push_back(std::move(kv.second));
      }
      buffered_watch_events_.clear();
    }

    LOG_INFO("[IndexOrchestrator] Reconciling {} buffered filesystem events captured during scan/rebuild.",
             batch.size());
    for (const auto &ev : batch) {
      if (bg_stopping_.load(std::memory_order_acquire)) {
        break;
      }
      ApplyLiveFileChange(ev, /*from_reconcile=*/true);
    }
  }
}

void IndexOrchestrator::SyncLiveWatchesWithoutStop() {
  if (!watch_engine_ || !config_ || !watch_engine_->is_watching()) {
    return;
  }
  const auto configured_dirs = config_->GetIndexedDirectories();
  const auto watched_dirs = watch_engine_->GetWatchedDirectories();
  std::unordered_set<std::string> active_keys;
  active_keys.reserve(configured_dirs.size());
  for (const auto &d : configured_dirs) {
    active_keys.insert(NormalizeDirKey(d));
  }

  for (const auto &watched : watched_dirs) {
    if (active_keys.find(NormalizeDirKey(watched)) == active_keys.end()) {
      watch_engine_->RemoveWatch(watched);
    }
  }
  for (const auto &dir : configured_dirs) {
    watch_engine_->AddWatch(dir);
  }
}

size_t IndexOrchestrator::DoPerformInitialScan(ScanProgressCallback progress_cb) {
  std::lock_guard<std::mutex> scan_lock(scan_mutex_);
  return PerformScanInternal(std::move(progress_cb), /*manage_buffering=*/true, /*manage_phase=*/true);
}

void IndexOrchestrator::SweepOfflineDeletedFiles(const std::unordered_set<int64_t> &visited_doc_ids,
                                                 const std::vector<std::string> &traversed_root_prefixes) {
  if (cancel_scan_.load(std::memory_order_acquire) || bg_stopping_.load(std::memory_order_acquire) ||
      traversed_root_prefixes.empty() || !name_index_) {
    return;
  }

  std::vector<int64_t> offline_deleted_ids;
  std::vector<std::string> offline_deleted_paths;
  const auto summaries = name_index_->SnapshotFileSummaries();
  for (const auto &item : summaries) {
    if (visited_doc_ids.find(item.doc_id) != visited_doc_ids.end()) {
      continue;
    }
    bool under_traversed_root = false;
    for (const auto &root_slash : traversed_root_prefixes) {
      if (PathStartsWithNormalizedDir(item.file_path, root_slash)) {
        under_traversed_root = true;
        break;
      }
    }
    if (!under_traversed_root) {
      continue;
    }
    std::error_code exists_ec;
    const bool still_exists = std::filesystem::exists(common::EncodingUtils::PathFromUtf8(item.file_path), exists_ec);
    if (!exists_ec && !still_exists) {
      offline_deleted_ids.push_back(item.doc_id);
      offline_deleted_paths.push_back(item.file_path);
    }
  }

  if (offline_deleted_ids.empty()) {
    return;
  }

  if (storage_engine_) {
    storage_engine_->DeleteDocumentsBatch(offline_deleted_ids);
  }
  name_index_->RemoveBatch(offline_deleted_ids);
  {
    std::lock_guard<std::mutex> flock(failed_files_mutex_);
    for (const auto &p : offline_deleted_paths) {
      failed_files_.erase(p);
    }
  }
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_files_deleted += offline_deleted_ids.size();
  }
  LOG_INFO("[IndexOrchestrator] Mark-and-sweep pruned {} offline-deleted files.", offline_deleted_ids.size());
}

void IndexOrchestrator::CollectScanCandidates(const std::vector<std::string> &watched_dirs,
                                              std::vector<std::string> *files_to_index,
                                              std::unordered_set<int64_t> *visited_doc_ids,
                                              std::vector<std::string> *traversed_root_prefixes) {
  std::unordered_set<DirFingerprint, DirFingerprintHash> visited_dirs;
  for (const auto &dir : watched_dirs) {
    if (cancel_scan_.load()) {
      break;
    }
    try {
      auto target_p = common::EncodingUtils::PathFromUtf8(dir);
      std::error_code root_ec;
      if (!std::filesystem::is_directory(target_p, root_ec)) {
        continue;
      }
      DirFingerprint root_fp;
      if (GetDirFingerprint(target_p, &root_fp)) {
        if (!visited_dirs.insert(root_fp).second) {
          LOG_INFO("[IndexOrchestrator] Watched directory '{}' is already scanned or a bind mount mirror, skipping.",
                   dir);
          continue;
        }
      }

      bool dir_completed = true;
      for (auto it = std::filesystem::recursive_directory_iterator(
               target_p, std::filesystem::directory_options::skip_permission_denied);
           it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (cancel_scan_.load()) {
          LOG_INFO("[IndexOrchestrator] Initial scan cancelled by user during candidate file discovery.");
          dir_completed = false;
          break;
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
          visited_doc_ids->insert(GenerateDocId(fpath));
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
            files_to_index->push_back(fpath);
          }
        }
      }
      if (dir_completed) {
        traversed_root_prefixes->push_back(NormalizeDirPrefixSlash(dir));
      }
    } catch (const std::exception &e) {
      LOG_WARN("[IndexOrchestrator] Failed to scan watched directory '{}': {}", dir, e.what());
    }
  }
}

size_t IndexOrchestrator::PerformScanInternal(ScanProgressCallback progress_cb, bool manage_buffering,
                                              bool manage_phase, bool force_ocr_reindex) {
  if (bg_stopping_.load()) {
    return 0;
  }
  cancel_scan_.store(false);

  ScopedPhaseResetGuard phase_guard(&phase_, OrchestratorPhase::kScanning, manage_phase);

  if (manage_buffering) {
    EnableWatchBuffering(/*clear_existing=*/false);
  }
  struct BufferingGuard {
    explicit BufferingGuard(IndexOrchestrator *self, bool active) : self_(self), active_(active) {}
    ~BufferingGuard() {
      if (active_ && self_) {
        self_->ReconcileAndDisableWatchBuffering();
      }
    }
    IndexOrchestrator *self_;
    bool active_;
  } buffering_guard(this, manage_buffering);

  std::vector<std::string> watched_dirs = config_ ? config_->GetIndexedDirectories() : std::vector<std::string>{};
  if (watched_dirs.empty() && watch_engine_) {
    watched_dirs = watch_engine_->GetWatchedDirectories();
  }

  std::vector<std::string> files_to_index;
  std::unordered_set<int64_t> visited_doc_ids;
  std::vector<std::string> traversed_root_prefixes;
  CollectScanCandidates(watched_dirs, &files_to_index, &visited_doc_ids, &traversed_root_prefixes);

  if (cancel_scan_.load()) {
    return 0;
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
        const bool force_file =
            force_ocr_reindex && IsEmbeddedOcrCandidateExt(ExtractExtension(ExtractFileName(file_path)));
        if (IndexSingleFile(file_path, force_file)) {
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

  // Mark-and-sweep offline reconciliation: prune files missing on disk under traversed watched roots.
  SweepOfflineDeletedFiles(visited_doc_ids, traversed_root_prefixes);

  if (manage_buffering && !cancel_scan_.load(std::memory_order_acquire) &&
      !bg_stopping_.load(std::memory_order_acquire) && rebuild_incomplete_.load(std::memory_order_acquire)) {
    SetRebuildIncomplete(false);
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
  return RebuildIndexInternal(std::move(progress_cb), /*manage_phase=*/true);
}

bool IndexOrchestrator::RebuildIndexInternal(ScanProgressCallback progress_cb, bool manage_phase) {
  if (bg_stopping_.load()) {
    return false;
  }
  LOG_INFO("[IndexOrchestrator] Starting complete index rebuild (watch-first with dirty buffer)...");
  cancel_scan_.store(false);

  ScopedPhaseResetGuard phase_guard(&phase_, OrchestratorPhase::kRebuilding, manage_phase);

  // Stop background space reclamation before clearing storage so vacuum and ClearAll never collide.
  StopAndJoinReclaim();

  // 1. Keep live watches armed (watch-first) so mutations during rebuild are captured cleanly in the
  //    dirty buffer instead of lost during a blind window. Drop stale pre-rebuild debounced items first.
  const bool was_watching = watch_engine_ && watch_engine_->is_watching();
  if (debouncer_) {
    debouncer_->ClearPending();
  }
  if (was_watching) {
    SyncLiveWatchesWithoutStop();
  }
  EnableWatchBuffering(/*clear_existing=*/true);

  struct RebuildBufferingGuard {
    explicit RebuildBufferingGuard(IndexOrchestrator *self) : self_(self) {}
    ~RebuildBufferingGuard() {
      if (self_) {
        self_->ReconcileAndDisableWatchBuffering();
        self_->DoFlushWriteQueue();
      }
    }
    IndexOrchestrator *self_;
  } rebuild_buffering_guard(this);

  // 2. Mark rebuild as incomplete BEFORE clearing storage so cancelled/crashed rebuilds self-heal on recovery.
  if (write_queue_) {
    write_queue_->Pause();
  } else {
    DoFlushWriteQueue();
  }
  SetRebuildIncomplete(true);

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

  // 5. Perform clean initial scan (buffering managed across ClearAll + Scan + Reconcile).
  size_t indexed = PerformScanInternal(std::move(progress_cb), /*manage_buffering=*/false, /*manage_phase=*/false);

  if (cancel_scan_.load()) {
    LOG_INFO("[IndexOrchestrator] Rebuild cancelled after indexing {} files (rebuild_incomplete remains set).", indexed);
    return false;
  }
  SetRebuildIncomplete(false);
  LOG_INFO("[IndexOrchestrator] Rebuild completed successfully. Total files indexed: {}", indexed);
  return true;
}

void IndexOrchestrator::ScanDirectoryRecursive(const std::string &dir_path, bool force_reindex) {
  if (cancel_scan_.load() || bg_stopping_.load()) {
    return;
  }
  if (config_ && config_->IsDirectoryExcluded(dir_path)) {
    return;
  }
  try {
    auto target_p = common::EncodingUtils::PathFromUtf8(dir_path);
    for (auto it = std::filesystem::recursive_directory_iterator(
             target_p, std::filesystem::directory_options::skip_permission_denied);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (cancel_scan_.load() || bg_stopping_.load()) break;

      if (it->is_directory()) {
        std::string sub_dir = common::EncodingUtils::PathToUtf8(it->path());
        if (config_ && config_->IsDirectoryExcluded(sub_dir)) {
          it.disable_recursion_pending();
          continue;
        }
      }
      if (it->is_regular_file()) {
        std::string fpath = common::EncodingUtils::PathToUtf8(it->path());
        IndexSingleFile(fpath, force_reindex);
      }
    }
  } catch (const std::exception &e) {
    LOG_WARN("[IndexOrchestrator] Error walking directory '{}': {}", dir_path, e.what());
  }
}

bool IndexOrchestrator::IndexSingleFile(const std::string &file_path, bool force_reindex) {
  std::lock_guard<std::mutex> path_lock(LockBucketForPath(file_path));
  return IndexSingleFileLocked(file_path, force_reindex);
}

bool IndexOrchestrator::IndexSingleFileLocked(const std::string &file_path, bool force_reindex) {
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
  if (!force_reindex) {
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

  if (!force_reindex && meta_found && db_size == file_size && db_mtime == file_mtime) {
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
  std::unique_lock<std::mutex> dispatch_lock(live_dispatch_mutex_);
  if (buffer_watch_events_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> buf_lock(buffered_events_mutex_);
    RecordBufferedEventUnlocked(event);
    return true;
  }
  return ApplyLiveFileChange(event, /*from_reconcile=*/false);
}

bool IndexOrchestrator::ApplyLiveFileChange(const DebouncedEvent &event, bool from_reconcile) {
  auto delete_path_or_subtree = [&](const std::string &target_path) {
    bool handled_as_single_file = false;
    {
      std::lock_guard<std::mutex> path_lock(LockBucketForPath(target_path));
      const int64_t doc_id = GenerateDocId(target_path);
      uint64_t dummy_size = 0;
      int64_t dummy_mtime = 0;
      const bool exact_known =
          (name_index_ && name_index_->GetMetadata(doc_id, &dummy_size, &dummy_mtime)) ||
          (storage_engine_ && storage_engine_->GetFileMetadata(doc_id, &dummy_size, &dummy_mtime));
      const bool is_subtree = !exact_known && name_index_ && name_index_->ContainsPathOrPrefix(target_path);
      if (!is_subtree) {
        handled_as_single_file = true;
        LOG_INFO("[IndexOrchestrator] File delete event: '{}' | removing from index", target_path);
        if (name_index_) {
          name_index_->Remove(doc_id);
        }
        {
          std::lock_guard<std::mutex> flock(failed_files_mutex_);
          failed_files_.erase(target_path);
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
      }
    }

    if (!handled_as_single_file) {
      LOG_INFO("[IndexOrchestrator] Directory delete event: '{}' | removing subtree from index", target_path);
      if (write_queue_) {
        write_queue_->Pause();
      } else {
        DoFlushWriteQueue();
      }
      int64_t deleted_count = 0;
      if (storage_engine_) {
        std::vector<int64_t> deleted_ids;
        deleted_count = storage_engine_->DeleteDirectory(target_path, &deleted_ids);
      }
      if (name_index_) {
        name_index_->RemoveDirectoryPrefix(target_path);
      }
      if (write_queue_) {
        write_queue_->Resume();
      }
      PurgeFailedFilesUnder(target_path);
      if (deleted_count > 0) {
        {
          std::lock_guard<std::mutex> lock(stats_mutex_);
          stats_.total_files_deleted += deleted_count;
        }
        DoTriggerAsyncSpaceReclaim();
      }
    }
  };

  auto add_or_modify_path = [&](const std::string &target_path, const char *label) -> bool {
    std::error_code ec;
    const auto fs_path = common::EncodingUtils::PathFromUtf8(target_path);
    if (std::filesystem::is_directory(fs_path, ec)) {
      if (config_ && !config_->IsDirectoryExcluded(target_path)) {
        LOG_INFO("[IndexOrchestrator] Directory {} event: '{}' | scanning subtree", label, target_path);
        ScanDirectoryRecursive(target_path, /*force_reindex=*/from_reconcile);
      }
      return true;
    }
    LOG_INFO("[IndexOrchestrator] File {} event: '{}' | triggering index", label, target_path);
    std::lock_guard<std::mutex> path_lock(LockBucketForPath(target_path));
    return IndexSingleFileLocked(target_path, /*force_reindex=*/from_reconcile);
  };

  if (event.action == FileChangeAction::kDelete) {
    delete_path_or_subtree(event.file_path);
    return true;
  }

  if (event.action == FileChangeAction::kMoved) {
    if (!event.old_path.empty()) {
      delete_path_or_subtree(event.old_path);
    }
    if (!event.file_path.empty()) {
      std::error_code ec;
      if (std::filesystem::exists(common::EncodingUtils::PathFromUtf8(event.file_path), ec)) {
        return add_or_modify_path(event.file_path, "moved");
      }
    }
    return true;
  }

  if (event.action == FileChangeAction::kAdd) {
    return add_or_modify_path(event.file_path, "added");
  }

  if (event.action == FileChangeAction::kModified) {
    return add_or_modify_path(event.file_path, "modified");
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
    pending_diff_ = ConfigChangeSummary{};
    pending_requires_scan_ = false;
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
  if (debouncer_) {
    debouncer_->set_debounce_ms(config_->debounce_ms());
  }
}

size_t IndexOrchestrator::DoPruneStaleEntriesByConfig() {
  std::lock_guard<std::mutex> scan_lock(scan_mutex_);
  return PruneStaleEntriesByConfigLocked();
}

size_t IndexOrchestrator::PruneStaleEntriesByConfigLocked() {
  if (!config_ || !name_index_) {
    return 0;
  }

  const auto summaries = name_index_->SnapshotFileSummaries();
  if (summaries.empty()) {
    return 0;
  }

  const auto configured_dirs = config_->GetIndexedDirectories();
  std::vector<std::string> dir_prefixes;
  dir_prefixes.reserve(configured_dirs.size());
  for (const auto &d : configured_dirs) {
    if (!d.empty()) {
      dir_prefixes.push_back(NormalizeDirPrefixSlash(d));
    }
  }

  std::vector<int64_t> stale_doc_ids;
  std::vector<std::string> stale_paths;
  for (const auto &item : summaries) {
    bool inside_dir = false;
    for (const auto &prefix : dir_prefixes) {
      if (PathStartsWithNormalizedDir(item.file_path, prefix)) {
        inside_dir = true;
        break;
      }
    }
    if (!inside_dir || !config_->ShouldIndex(item.file_path, item.ext, item.file_size)) {
      stale_doc_ids.push_back(item.doc_id);
      stale_paths.push_back(item.file_path);
    }
  }

  if (stale_doc_ids.empty()) {
    return 0;
  }

  if (write_queue_) {
    write_queue_->Pause();
  } else {
    DoFlushWriteQueue();
  }

  if (storage_engine_) {
    storage_engine_->DeleteDocumentsBatch(stale_doc_ids);
  }
  name_index_->RemoveBatch(stale_doc_ids);

  if (write_queue_) {
    write_queue_->Resume();
  }

  {
    std::lock_guard<std::mutex> flock(failed_files_mutex_);
    for (const auto &p : stale_paths) {
      failed_files_.erase(p);
    }
  }
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.total_files_deleted += static_cast<uint64_t>(stale_doc_ids.size());
  }
  DoTriggerAsyncSpaceReclaim();
  LOG_INFO("[IndexOrchestrator] Pruned {} stale index entries matching updated config rules.", stale_doc_ids.size());
  return stale_doc_ids.size();
}

void IndexOrchestrator::DoSyncWatchedDirectories() {
  if (!watch_engine_ || !config_) {
    return;
  }
  const auto configured_dirs = config_->GetIndexedDirectories();
  const auto watched_dirs = watch_engine_->GetWatchedDirectories();
  std::unordered_set<std::string> active_keys;
  active_keys.reserve(configured_dirs.size());
  for (const auto &d : configured_dirs) {
    active_keys.insert(NormalizeDirKey(d));
  }

  for (const auto &watched : watched_dirs) {
    if (bg_stopping_.load()) {
      break;
    }
    if (active_keys.find(NormalizeDirKey(watched)) == active_keys.end()) {
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
    pending_diff_ = ConfigChangeSummary{};
    pending_requires_scan_ = false;
  }
  cancel_scan_.store(true);
  if (bg_task_thread_.joinable()) {
    bg_task_thread_.join();
  }
  std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
  bg_task_running_ = false;
  pending_task_ = PendingTask::kNone;
  pending_diff_ = ConfigChangeSummary{};
  pending_requires_scan_ = false;
  phase_.store(OrchestratorPhase::kIdle, std::memory_order_release);
}

void IndexOrchestrator::EnqueueOrLaunchBackgroundTask(IndexTaskKind initial_kind, PendingTask req_pending,
                                                      ConfigChangeSummary req_diff, bool cancel_active_non_rebuild,
                                                      BackgroundIndexCallbacks callbacks) {
  std::lock_guard<std::mutex> join_lock(bg_join_mutex_);
  if (bg_stopping_.load()) {
    return;
  }

  {
    std::lock_guard<std::mutex> cb_lock(bg_callback_mutex_);
    bg_callbacks_ = std::move(callbacks);
  }

  auto merge_diff = [](ConfigChangeSummary &dst, const ConfigChangeSummary &src) {
    dst.runtime_changed = dst.runtime_changed || src.runtime_changed;
    dst.debounce_changed = dst.debounce_changed || src.debounce_changed;
    dst.rules_narrowed = dst.rules_narrowed || src.rules_narrowed;
    dst.rules_expanded = dst.rules_expanded || src.rules_expanded;
    dst.ocr_expanded = dst.ocr_expanded || src.ocr_expanded;
    for (const auto &d : src.removed_dirs) {
      if (std::find(dst.removed_dirs.begin(), dst.removed_dirs.end(), d) == dst.removed_dirs.end()) {
        dst.removed_dirs.push_back(d);
      }
    }
    for (const auto &d : src.added_dirs) {
      if (std::find(dst.added_dirs.begin(), dst.added_dirs.end(), d) == dst.added_dirs.end()) {
        dst.added_dirs.push_back(d);
      }
    }
  };

  {
    std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
    if (bg_task_running_) {
      merge_diff(pending_diff_, req_diff);
      // Preserve higher-priority follow-up task and never cancel/downgrade an active kRebuild.
      if (static_cast<int>(req_pending) > static_cast<int>(pending_task_)) {
        pending_task_ = req_pending;
      }
      if (cancel_active_non_rebuild && active_kind_ != IndexTaskKind::kRebuild) {
        if (active_kind_ == IndexTaskKind::kInitial || active_kind_ == IndexTaskKind::kIncremental) {
          // Ensure an interrupted scan continues to completion during follow-up kConfigSync.
          pending_requires_scan_ = true;
        }
        cancel_scan_.store(true);
      }
      return;
    }
  }

  // Previous task already cleared bg_task_running_ = false and is exiting; join safely under bg_join_mutex_.
  if (bg_task_thread_.joinable()) {
    bg_task_thread_.join();
  }

  uint64_t initial_generation = 0;
  {
    std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
    if (bg_stopping_.load()) {
      return;
    }
    bg_task_running_ = true;
    active_kind_ = initial_kind;
    pending_task_ = PendingTask::kNone;
    pending_diff_ = ConfigChangeSummary{};
    pending_requires_scan_ = false;
    initial_generation = bg_generation_.fetch_add(1) + 1;
  }

  bg_task_thread_ = std::thread(&IndexOrchestrator::BackgroundTaskLoop, this, initial_kind, initial_generation,
                                req_pending, std::move(req_diff), /*initial_force_rescan=*/false);
}

void IndexOrchestrator::DoApplyConfigChanges(const ConfigChangeSummary &diff, BackgroundIndexCallbacks callbacks) {
  DoApplyRuntimeConfig();
  if (!diff.HasStructuralChanges()) {
    return;
  }
  EnqueueOrLaunchBackgroundTask(IndexTaskKind::kIncremental, PendingTask::kConfigSync, diff,
                                /*cancel_active_non_rebuild=*/true, std::move(callbacks));
}

void IndexOrchestrator::DoStartBackgroundTask(bool rebuild, bool force_incremental,
                                              BackgroundIndexCallbacks callbacks) {
  const bool is_cold = !rebuild && !force_incremental && (!name_index_ || name_index_->size() == 0);
  const IndexTaskKind initial_kind =
      rebuild ? IndexTaskKind::kRebuild : (is_cold ? IndexTaskKind::kInitial : IndexTaskKind::kIncremental);
  const PendingTask req_pending = rebuild ? PendingTask::kRebuild : PendingTask::kIncremental;
  EnqueueOrLaunchBackgroundTask(initial_kind, req_pending, ConfigChangeSummary{},
                                /*cancel_active_non_rebuild=*/true, std::move(callbacks));
}

void IndexOrchestrator::BackgroundTaskLoop(IndexTaskKind initial_kind, uint64_t initial_generation,
                                           PendingTask initial_task_mode, ConfigChangeSummary initial_diff,
                                           bool initial_force_rescan) {
  IndexTaskKind kind = initial_kind;
  uint64_t generation = initial_generation;
  PendingTask task_mode = initial_task_mode;
  ConfigChangeSummary diff = std::move(initial_diff);
  bool force_rescan = initial_force_rescan;

  while (true) {
    {
      std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
      if (bg_stopping_.load()) {
        bg_task_running_ = false;
        pending_task_ = PendingTask::kNone;
        pending_diff_ = ConfigChangeSummary{};
        pending_requires_scan_ = false;
        phase_.store(OrchestratorPhase::kIdle, std::memory_order_release);
        break;
      }
      active_kind_ = kind;
      cancel_scan_.store(false);
    }

    phase_.store((kind == IndexTaskKind::kRebuild) ? OrchestratorPhase::kRebuilding : OrchestratorPhase::kScanning,
                 std::memory_order_release);

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
      std::lock_guard<std::mutex> scan_lock(scan_mutex_);
      ok = RebuildIndexInternal(std::move(progress_forwarder), /*manage_phase=*/false);
      count = ok ? DoIndexedFileCount() : 0;
    } else if (task_mode == PendingTask::kConfigSync) {
      for (const auto &removed : diff.removed_dirs) {
        if (bg_stopping_.load()) {
          break;
        }
        DoRemoveDirectoryIndex(removed);
      }
      DoSyncWatchedDirectories();
      if (!bg_stopping_.load() && diff.rules_narrowed) {
        DoPruneStaleEntriesByConfig();
      }
      const bool needs_scan = force_rescan || !diff.added_dirs.empty() || diff.rules_expanded ||
                              rebuild_incomplete_.load(std::memory_order_acquire);
      if (needs_scan && !cancel_scan_.load() && !bg_stopping_.load()) {
        std::lock_guard<std::mutex> scan_lock(scan_mutex_);
        PerformScanInternal(std::move(progress_forwarder), /*manage_buffering=*/true, /*manage_phase=*/false,
                            /*force_ocr_reindex=*/diff.ocr_expanded);
      }
      count = DoIndexedFileCount();
    } else {
      DoSyncWatchedDirectories();
      if (!cancel_scan_.load() && !bg_stopping_.load()) {
        std::lock_guard<std::mutex> scan_lock(scan_mutex_);
        count = PerformScanInternal(std::move(progress_forwarder), /*manage_buffering=*/true, /*manage_phase=*/false);
      }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
    const bool cancelled = cancel_scan_.load();

    PendingTask next_pending = PendingTask::kNone;
    uint64_t next_generation = 0;
    ConfigChangeSummary next_diff;
    bool next_force_rescan = false;
    {
      std::lock_guard<std::mutex> task_lock(bg_task_mutex_);
      if (bg_stopping_.load()) {
        pending_task_ = PendingTask::kNone;
        pending_diff_ = ConfigChangeSummary{};
        pending_requires_scan_ = false;
      }
      next_pending = pending_task_;
      next_diff = std::move(pending_diff_);
      next_force_rescan = pending_requires_scan_;
      pending_task_ = PendingTask::kNone;
      pending_diff_ = ConfigChangeSummary{};
      pending_requires_scan_ = false;
      if (next_pending == PendingTask::kNone) {
        bg_task_running_ = false;
        phase_.store(OrchestratorPhase::kIdle, std::memory_order_release);
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
    task_mode = next_pending;
    diff = std::move(next_diff);
    force_rescan = next_force_rescan;
  }
}

}  // namespace service
}  // namespace soeasy
