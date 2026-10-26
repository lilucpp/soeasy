#include "service/event_debouncer.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace soeasy {
namespace service {

EventDebouncer::EventDebouncer(uint32_t debounce_ms)
    : debounce_ms_(debounce_ms) {}

EventDebouncer::~EventDebouncer() {
  Stop(false);
}

void EventDebouncer::SetCallback(ChangeCallback cb) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(cb);
}

void EventDebouncer::Start() {
  if (is_running_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  flush_thread_ = std::thread(&EventDebouncer::FlushLoop, this);
}

void EventDebouncer::Stop(bool flush_pending) {
  if (!is_running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  cv_.notify_all();
  if (flush_thread_.joinable()) {
    flush_thread_.join();
  }

  if (flush_pending) {
    Flush();
  }
}

void EventDebouncer::PushEvent(const std::string& path, FileChangeAction action,
                               const std::string& old_path) {
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);

  if (action == FileChangeAction::kMoved && !old_path.empty()) {
    // Treat Move as Delete on old_path and Add on path
    auto it_old = pending_events_.find(old_path);
    if (it_old != pending_events_.end() &&
        it_old->second.action == FileChangeAction::kAdd) {
      pending_events_.erase(it_old);
    } else {
      pending_events_[old_path] =
          DebouncedEvent{old_path, "", FileChangeAction::kDelete, now};
    }
    pending_events_[path] =
        DebouncedEvent{path, old_path, FileChangeAction::kAdd, now};
    cv_.notify_one();
    return;
  }

  auto it = pending_events_.find(path);
  if (it != pending_events_.end()) {
    FileChangeAction prev_action = it->second.action;
    it->second.last_event_time = now;

    if (prev_action == FileChangeAction::kAdd) {
      if (action == FileChangeAction::kDelete) {
        // Created and deleted within debounce window -> discard both
        pending_events_.erase(it);
        return;
      }
      // If Modified follows Add, it is still an Add operation
      return;
    }

    if (prev_action == FileChangeAction::kModified &&
        action == FileChangeAction::kDelete) {
      it->second.action = FileChangeAction::kDelete;
      return;
    }

    if (prev_action == FileChangeAction::kDelete &&
        action == FileChangeAction::kAdd) {
      it->second.action = FileChangeAction::kModified;
      return;
    }

    it->second.action = action;
  } else {
    pending_events_[path] = DebouncedEvent{path, old_path, action, now};
  }

  cv_.notify_one();
}

void EventDebouncer::Flush() {
  std::vector<DebouncedEvent> ready_to_flush;
  ChangeCallback cb;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    cb = callback_;
    for (auto& pair : pending_events_) {
      ready_to_flush.push_back(std::move(pair.second));
    }
    pending_events_.clear();
  }

  if (cb) {
    for (const auto& ev : ready_to_flush) {
      cb(ev);
    }
  }
}

void EventDebouncer::FlushLoop() {
  while (is_running_.load(std::memory_order_acquire)) {
    std::vector<DebouncedEvent> ready_to_flush;
    ChangeCallback cb;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(50));

      if (!is_running_.load(std::memory_order_acquire)) {
        break;
      }

      auto now = std::chrono::steady_clock::now();
      auto delay = std::chrono::milliseconds(debounce_ms_);

      for (auto it = pending_events_.begin(); it != pending_events_.end();) {
        if (now - it->second.last_event_time >= delay) {
          ready_to_flush.push_back(std::move(it->second));
          it = pending_events_.erase(it);
        } else {
          ++it;
        }
      }

      cb = callback_;
    }

    if (cb && !ready_to_flush.empty()) {
      for (const auto& ev : ready_to_flush) {
        cb(ev);
      }
    }
  }
}

}  // namespace service
}  // namespace soeasy
