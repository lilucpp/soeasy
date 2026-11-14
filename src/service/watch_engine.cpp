#include "service/watch_engine.h"

#include <cerrno>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/encoding_utils.h"
#include "common/logger.h"
#include "common/system_env.h"
#include <cerrno>

namespace soeasy {
namespace service {

WatchEngine::WatchEngine(std::shared_ptr<IndexConfig> config, std::shared_ptr<EventDebouncer> debouncer)
    : config_(std::move(config)), debouncer_(std::move(debouncer)) {
  file_watcher_ = std::make_unique<efsw::FileWatcher>();
}

WatchEngine::~WatchEngine() {
  Stop();
}

std::string WatchEngine::CombinePath(const std::string &dir, const std::string &filename) {
  if (dir.empty()) {
    return filename;
  }
  if (filename.empty()) {
    return dir;
  }
  if (dir.back() == '/' || dir.back() == '\\') {
    return dir + filename;
  }
  return dir + "/" + filename;
}

bool WatchEngine::Start() {
  std::lock_guard<std::mutex> lock(watch_mutex_);
  if (is_watching_) {
    return true;
  }

  if (debouncer_) {
    debouncer_->Start();
  }

  file_watcher_->watch();
  is_watching_ = true;
  return true;
}

void WatchEngine::Stop() {
  std::lock_guard<std::mutex> lock(watch_mutex_);
  if (!is_watching_) {
    return;
  }

  if (debouncer_) {
    debouncer_->Stop(false);
  }

  for (const auto &pair : watch_ids_) {
    file_watcher_->removeWatch(pair.second);
  }
  watch_ids_.clear();
  is_watching_ = false;
}

bool WatchEngine::AddWatch(const std::string &raw_dir_path) {
  std::string dir_path = common::EncodingUtils::ToUtf8(raw_dir_path);
  std::lock_guard<std::mutex> lock(watch_mutex_);

  auto it = watch_ids_.find(dir_path);
  if (it != watch_ids_.end()) {
    return true;  // Already watched
  }

  efsw::WatchID watch_id = file_watcher_->addWatch(dir_path, this, true);
  if (watch_id <= 0) {
    LOG_ERROR("[WatchEngine] Failed to add watch on '{}'", dir_path);
#if defined(__linux__)
    if (errno == ENOSPC) {
      auto info = common::SystemEnv::CheckAndOptimizeInotifyWatches();
      LOG_WARN("[WatchEngine] Inotify limit exhausted (ENOSPC). Current limit: {}.{}",
               info.current_watches, common::SystemEnv::GetInotifyTuningGuide(info.current_watches));
    }
#endif
    return false;
  }

  watch_ids_[dir_path] = watch_id;
  if (config_) {
    config_->AddIndexedDirectory(dir_path);
  }
  LOG_INFO("[WatchEngine] Added watch on directory: '{}' (watch_id: {})", dir_path, watch_id);
  return true;
}

void WatchEngine::RemoveWatch(const std::string &dir_path) {
  std::lock_guard<std::mutex> lock(watch_mutex_);

  auto it = watch_ids_.find(dir_path);
  if (it != watch_ids_.end()) {
    file_watcher_->removeWatch(it->second);
    watch_ids_.erase(it);
    LOG_INFO("[WatchEngine] Removed watch on directory: '{}'", dir_path);
  }
  if (config_) {
    config_->RemoveIndexedDirectory(dir_path);
  }
}

void WatchEngine::SetPathKnownCallback(PathKnownCallback cb) {
  std::lock_guard<std::mutex> lock(watch_mutex_);
  path_known_cb_ = std::move(cb);
}

std::vector<std::string> WatchEngine::GetWatchedDirectories() const {
  std::lock_guard<std::mutex> lock(watch_mutex_);
  std::vector<std::string> dirs;
  dirs.reserve(watch_ids_.size());
  for (const auto &pair : watch_ids_) {
    dirs.push_back(pair.first);
  }
  return dirs;
}

void WatchEngine::handleFileAction(efsw::WatchID watchid, const std::string &raw_dir, const std::string &raw_filename,
                                   efsw::Action action, const std::string &raw_old_filename) {
  (void)watchid;
  std::string dir = common::EncodingUtils::ToUtf8(raw_dir);
  std::string filename = common::EncodingUtils::ToUtf8(raw_filename);
  std::string old_filename = common::EncodingUtils::ToUtf8(raw_old_filename);

  std::string full_path = CombinePath(dir, filename);
  std::string old_full_path;
  if (!old_filename.empty()) {
    old_full_path = CombinePath(dir, old_filename);
  }

  PathKnownCallback known_cb;
  {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    known_cb = path_known_cb_;
  }

  // 1. Exclude check: if moved from non-excluded -> excluded, emit Delete on old path; otherwise ignore excluded targets.
  const bool new_excluded = config_ && config_->IsDirectoryExcluded(full_path);
  const bool old_excluded = !old_full_path.empty() && config_ && config_->IsDirectoryExcluded(old_full_path);
  if (new_excluded) {
    if (action == efsw::Actions::Moved && !old_full_path.empty() && !old_excluded) {
      const bool old_relevant =
          config_->IsFileSupported(old_filename) || (known_cb && known_cb(old_full_path));
      if (old_relevant && debouncer_) {
        LOG_INFO("[WatchEngine] File/dir moved into excluded path: '{}' -> '{}', deleting old index.", old_full_path,
                 full_path);
        debouncer_->PushEvent(old_full_path, FileChangeAction::kDelete, "");
      }
    } else {
      LOG_DEBUG("[WatchEngine] Ignored excluded filesystem event: '{}'", full_path);
    }
    return;
  }

  // 2. Relevance check: match supported extension, known indexed file/directory subtree on Delete/Moved,
  //    or newly added/moved-in directories.
  if (config_) {
    bool supported = config_->IsFileSupported(filename);
    if (!supported && !old_filename.empty()) {
      supported = config_->IsFileSupported(old_filename);
    }
    if (!supported && (action == efsw::Actions::Delete || action == efsw::Actions::Moved) && known_cb) {
      if (known_cb(full_path) || (!old_full_path.empty() && known_cb(old_full_path))) {
        supported = true;
      }
    }
    if (!supported && (action == efsw::Actions::Add || action == efsw::Actions::Moved)) {
      std::error_code ec;
      if (std::filesystem::is_directory(common::EncodingUtils::PathFromUtf8(full_path), ec)) {
        supported = true;
      }
    }
    if (!supported) {
      LOG_DEBUG("[WatchEngine] Ignored unsupported file event: '{}'", filename);
      return;
    }
  }

  FileChangeAction mapped_action = FileChangeAction::kModified;
  const char *action_str = "MODIFIED";
  switch (action) {
    case efsw::Actions::Add:
      mapped_action = FileChangeAction::kAdd;
      action_str = "ADD";
      break;
    case efsw::Actions::Delete:
      mapped_action = FileChangeAction::kDelete;
      action_str = "DELETE";
      break;
    case efsw::Actions::Modified:
      mapped_action = FileChangeAction::kModified;
      action_str = "MODIFIED";
      break;
    case efsw::Actions::Moved:
      mapped_action = FileChangeAction::kMoved;
      action_str = "MOVED";
      break;
    default:
      mapped_action = FileChangeAction::kModified;
      action_str = "UNKNOWN";
      break;
  }

  LOG_INFO("[WatchEngine] File system change detected: [{}] path: '{}'{}", action_str, full_path,
           old_full_path.empty() ? "" : (" (from: " + old_full_path + ")"));

  if (debouncer_) {
    debouncer_->PushEvent(full_path, mapped_action, old_full_path);
  }
}

}  // namespace service
}  // namespace soeasy
