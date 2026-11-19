// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "service/watch_reconciler.h"

namespace soeasy {
namespace service {

void WatchReconciler::EnableBuffering(bool clear_existing) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (clear_existing) {
    buffered_events_.clear();
  }
  is_buffering_.store(true, std::memory_order_release);
}

bool WatchReconciler::IsBufferingEnabled() const {
  return is_buffering_.load(std::memory_order_acquire);
}

void WatchReconciler::RecordEvent(const DebouncedEvent &event) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto merge_single = [&](const std::string &path, FileChangeAction action, const std::string &old_path) {
    auto it = buffered_events_.find(path);
    if (it == buffered_events_.end()) {
      buffered_events_[path] = DebouncedEvent{path, old_path, action, event.last_event_time};
      return;
    }
    it->second.last_event_time = event.last_event_time;
    const FileChangeAction prev = it->second.action;
    if (action == FileChangeAction::kDelete) {
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

std::vector<DebouncedEvent> WatchReconciler::DrainBatch() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (buffered_events_.empty()) {
    return {};
  }
  std::vector<DebouncedEvent> batch;
  batch.reserve(buffered_events_.size());
  for (auto &kv : buffered_events_) {
    batch.push_back(std::move(kv.second));
  }
  buffered_events_.clear();
  return batch;
}

bool WatchReconciler::TryDisableBufferingIfEmpty(std::vector<DebouncedEvent> *out_remaining) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (buffered_events_.empty()) {
    is_buffering_.store(false, std::memory_order_release);
    return true;
  }
  if (out_remaining) {
    out_remaining->reserve(out_remaining->size() + buffered_events_.size());
    for (auto &kv : buffered_events_) {
      out_remaining->push_back(std::move(kv.second));
    }
    buffered_events_.clear();
  }
  return false;
}

void WatchReconciler::ForceDisableBuffering() {
  std::lock_guard<std::mutex> lock(mutex_);
  is_buffering_.store(false, std::memory_order_release);
  buffered_events_.clear();
}

}  // namespace service
}  // namespace soeasy
