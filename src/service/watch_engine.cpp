#include "service/watch_engine.h"

#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/logger.h"

namespace soeasy {
namespace service {

WatchEngine::WatchEngine(std::shared_ptr<IndexConfig> config,
                         std::shared_ptr<EventDebouncer> debouncer)
    : config_(std::move(config)), debouncer_(std::move(debouncer)) {
  file_watcher_ = std::make_unique<efsw::FileWatcher>();
}

WatchEngine::~WatchEngine() {
  Stop();
}

std::string WatchEngine::CombinePath(const std::string& dir,
                                     const std::string& filename) {
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

  for (const auto& pair : watch_ids_) {
    file_watcher_->removeWatch(pair.second);
  }
  watch_ids_.clear();
  is_watching_ = false;
}

bool WatchEngine::AddWatch(const std::string& dir_path) {
  std::lock_guard<std::mutex> lock(watch_mutex_);

  auto it = watch_ids_.find(dir_path);
  if (it != watch_ids_.end()) {
    return true;  // Already watched
  }

  efsw::WatchID watch_id = file_watcher_->addWatch(dir_path, this, true);
  if (watch_id <= 0) {
    LOG_ERROR("[WatchEngine] Failed to add watch on '{}'", dir_path);
    return false;
  }

  watch_ids_[dir_path] = watch_id;
  if (config_) {
    config_->AddIndexedDirectory(dir_path);
  }
  LOG_INFO("[WatchEngine] Added watch on directory: '{}' (watch_id: {})", dir_path, watch_id);
  return true;
}

void WatchEngine::RemoveWatch(const std::string& dir_path) {
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

std::vector<std::string> WatchEngine::GetWatchedDirectories() const {
  std::lock_guard<std::mutex> lock(watch_mutex_);
  std::vector<std::string> dirs;
  dirs.reserve(watch_ids_.size());
  for (const auto& pair : watch_ids_) {
    dirs.push_back(pair.first);
  }
  return dirs;
}

void WatchEngine::handleFileAction(efsw::WatchID watchid,
                                   const std::string& dir,
                                   const std::string& filename,
                                   efsw::Action action,
                                   const std::string& old_filename) {
  std::string full_path = CombinePath(dir, filename);

  // Check if directory or file path is excluded by configuration
  if (config_ && config_->IsDirectoryExcluded(full_path)) {
    LOG_DEBUG("[WatchEngine] Ignored excluded filesystem event: '{}'", full_path);
    return;
  }

  std::string old_full_path;
  if (!old_filename.empty()) {
    old_full_path = CombinePath(dir, old_filename);
  }

  FileChangeAction mapped_action = FileChangeAction::kModified;
  const char* action_str = "MODIFIED";
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

  LOG_INFO("[WatchEngine] File system change detected: [{}] path: '{}'{}",
           action_str, full_path,
           old_full_path.empty() ? "" : (" (from: " + old_full_path + ")"));

  if (debouncer_) {
    debouncer_->PushEvent(full_path, mapped_action, old_full_path);
  }
}

}  // namespace service
}  // namespace soeasy
