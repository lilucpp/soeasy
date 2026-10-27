#ifndef SOEASY_SRC_SERVICE_WORKER_GUARD_H_
#define SOEASY_SRC_SERVICE_WORKER_GUARD_H_

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/shm_pool.h"

namespace soeasy {
namespace service {

enum class TaskStatus {
  kPending = 0,
  kRunning = 1,
  kSuccess = 2,
  kTimeout = 3,
  kCrashed = 4,
  kFailed = 5,
};

struct ParseTask {
  int64_t doc_id = 0;
  std::string file_path;
  std::string ext;
  int parse_level = 1;
  uint32_t timeout_ms = 15000;  // Dynamic timeout computed by DocPrechecker
};

struct ParseTaskResult {
  int64_t doc_id = 0;
  TaskStatus status = TaskStatus::kFailed;
  uint32_t slot_id = 0;
  uint64_t extracted_bytes = 0;
  bool is_scanned_pdf = false;
  std::string error_message;
  double elapsed_ms = 0.0;
};

enum class WorkerState {
  kIdle = 0,
  kBusy = 1,
  kKilledByWatchdog = 2
};

struct WorkerInstance {
  pid_t pid = -1;
  int socket_fd = -1;
  WorkerState state = WorkerState::kIdle;
  int64_t current_doc_id = 0;
  uint32_t current_slot_id = 0;
  uint32_t current_timeout_ms = 15000;
  std::chrono::steady_clock::time_point task_start_time;
  uint64_t total_tasks_processed = 0;

  bool is_busy() const { return state == WorkerState::kBusy; }
};

class WorkerGuard {
 public:
  static constexpr uint32_t kDefaultTimeoutMs = 15000;          // 15s default
  static constexpr uint32_t kDefaultStallTimeoutMs = 5000;      // 5s stall heartbeat
  static constexpr size_t kDefaultMaxMemoryBytes = 512 * 1024 * 1024;  // 512MB RSS

  WorkerGuard();
  ~WorkerGuard();

  // Disallow copy and assign
  WorkerGuard(const WorkerGuard&) = delete;
  WorkerGuard& operator=(const WorkerGuard&) = delete;

  /**
   * @brief Initialize worker pool and shared memory pool.
   *
   * @param worker_bin_path Absolute path to soeasy-worker binary, or empty for in-process fork
   * @param worker_count Number of worker processes to spawn
   * @param timeout_ms Max execution time allowed per document before SIGKILL
   * @param shm_slot_count Number of SHM slots
   * @param shm_slot_capacity Capacity per slot in bytes
   * @return true on success
   */
  bool Init(const std::string& worker_bin_path, size_t worker_count = 2,
            uint32_t timeout_ms = kDefaultTimeoutMs,
            size_t shm_slot_count = common::ShmSlotPool::kDefaultSlotCount,
            size_t shm_slot_capacity = common::ShmSlotPool::kDefaultSlotCapacity);

  /**
   * @brief Terminate all workers, stop watchdog thread, and destroy SHM.
   */
  void Shutdown();

  /**
   * @brief Dispatch a file parse task to an available worker and block until complete or timeout.
   *
   * @param task Document parsing request
   * @param out_result Receives execution status, extracted size, and slot reference
   * @return true if execution completed (even if status is timeout/crash/failed)
   */
  bool ExecuteTask(const ParseTask& task, ParseTaskResult* out_result);

  /**
   * @brief Access shared memory slot pool for reading zero-copy extracted text.
   */
  common::ShmSlotPool* shm_pool() { return &shm_pool_; }

  size_t worker_count() const { return workers_.size(); }
  uint32_t timeout_ms() const { return timeout_ms_; }
  void set_timeout_ms(uint32_t ms) { timeout_ms_ = ms; }

 private:
  bool SpawnWorker(size_t index);
  void KillWorker(size_t index, TaskStatus status, const std::string& reason);
  void WatchdogLoop();
  static size_t GetProcessRssBytes(pid_t pid);

  std::string worker_bin_path_;
  uint32_t timeout_ms_ = kDefaultTimeoutMs;
  common::ShmSlotPool shm_pool_;

  std::vector<WorkerInstance> workers_;
  std::mutex pool_mutex_;
  std::condition_variable worker_available_cv_;

  std::atomic<bool> is_running_{false};
  std::thread watchdog_thread_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_WORKER_GUARD_H_
