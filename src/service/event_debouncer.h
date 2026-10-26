#ifndef SOEASY_SRC_SERVICE_EVENT_DEBOUNCER_H_
#define SOEASY_SRC_SERVICE_EVENT_DEBOUNCER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace soeasy {
namespace service {

enum class FileChangeAction {
  kAdd = 0,
  kDelete = 1,
  kModified = 2,
  kMoved = 3,
};

struct DebouncedEvent {
  std::string file_path;
  std::string old_path;  // For moved/renamed
  FileChangeAction action;
  std::chrono::steady_clock::time_point last_event_time;
};

using ChangeCallback = std::function<void(const DebouncedEvent& event)>;

class EventDebouncer {
 public:
  static constexpr uint32_t kDefaultDebounceMs = 300;

  explicit EventDebouncer(uint32_t debounce_ms = kDefaultDebounceMs);
  ~EventDebouncer();

  // Disallow copy and assign
  EventDebouncer(const EventDebouncer&) = delete;
  EventDebouncer& operator=(const EventDebouncer&) = delete;

  /**
   * @brief Set output callback invoked when debounced change settles.
   */
  void SetCallback(ChangeCallback cb);

  /**
   * @brief Push a new raw filesystem event into the debouncer.
   *
   * @param path Target file path
   * @param action Change action type
   * @param old_path Previous path if renamed/moved
   */
  void PushEvent(const std::string& path, FileChangeAction action,
                 const std::string& old_path = "");

  /**
   * @brief Start background debounce timer flush loop.
   */
  void Start();

  /**
   * @brief Stop background timer and optionally flush all pending events.
   */
  void Stop(bool flush_pending = false);

  /**
   * @brief Immediately flush all settled events (useful for testing and sync checkpoints).
   */
  void Flush();

  void set_debounce_ms(uint32_t ms) { debounce_ms_ = ms; }
  uint32_t debounce_ms() const { return debounce_ms_; }

 private:
  void FlushLoop();

  uint32_t debounce_ms_ = kDefaultDebounceMs;
  ChangeCallback callback_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::unordered_map<std::string, DebouncedEvent> pending_events_;

  std::atomic<bool> is_running_{false};
  std::thread flush_thread_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_EVENT_DEBOUNCER_H_
