#ifndef SOEASY_SRC_SERVICE_WORKER_GUARD_H_
#define SOEASY_SRC_SERVICE_WORKER_GUARD_H_

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "service/worker_process_utils.h"

namespace soeasy {
namespace service {

class WorkerRpcChannel;

enum class TaskStatus {
  kSuccess = 0,
  kTimeout = 1,
  kCrashed = 2,
  kFailed = 3,
};

struct ParseTask {
  int64_t doc_id = 0;
  std::string file_path;
  std::string ext;
  int parse_level = 1;
  uint32_t timeout_ms = 60000;  // Dynamic timeout computed by DocPrechecker
};

struct ParseTaskResult {
  int64_t doc_id = 0;
  TaskStatus status = TaskStatus::kFailed;
  uint64_t extracted_bytes = 0;
  bool is_scanned_pdf = false;
  std::string error_message;
  std::string extracted_text;  // Streamed directly over Cap'n Proto IPC
  size_t stream_chunk_count = 0;
  double elapsed_ms = 0.0;
};

enum class WorkerState { kIdle = 0, kBusy = 1, kKilledByWatchdog = 2, kCrashed = 3 };

struct DetachedWorkerResources {
  SubprocessHandle process;
  std::thread stdio_drain_thread;
  std::shared_ptr<WorkerRpcChannel> rpc_channel;

  ~DetachedWorkerResources() { Reset(); }
  DetachedWorkerResources() = default;
  DetachedWorkerResources(DetachedWorkerResources &&) noexcept = default;
  DetachedWorkerResources &operator=(DetachedWorkerResources &&other) noexcept;
  DetachedWorkerResources(const DetachedWorkerResources &) = delete;
  DetachedWorkerResources &operator=(const DetachedWorkerResources &) = delete;

  void Reset() noexcept;
};

struct WorkerInstance {
  SubprocessHandle process;
  std::thread stdio_drain_thread;
  WorkerPid pid = 0;
  std::shared_ptr<WorkerRpcChannel> rpc_channel;
  WorkerState state = WorkerState::kIdle;
  uint32_t current_timeout_ms = 60000;
  std::chrono::steady_clock::time_point task_start_time;
  std::chrono::steady_clock::time_point idle_start_time;

  WorkerInstance() = default;
  ~WorkerInstance();
  WorkerInstance(const WorkerInstance &) = delete;
  WorkerInstance &operator=(const WorkerInstance &) = delete;
  WorkerInstance(WorkerInstance &&) = delete;
  WorkerInstance &operator=(WorkerInstance &&) = delete;

  bool is_busy() const noexcept { return state == WorkerState::kBusy; }
  bool HasResources() const noexcept {
    return pid > 0 || process != nullptr || rpc_channel != nullptr || stdio_drain_thread.joinable();
  }

  DetachedWorkerResources Detach(WorkerState next_state = WorkerState::kIdle) noexcept;
};

struct WorkerLoggingOptions {
  std::string level = "info";
  bool enable_console = false;
  bool enable_sync = false;
  bool doctotext_logging = false;
  std::string doctotext_log_path;
  bool embedded_image_ocr = false;
  std::string worker_log_dir;
};

class WorkerGuard {
 public:
  static constexpr uint32_t kDefaultTimeoutMs = 60000;                 // 60s default
  static constexpr uint32_t kDefaultIdleEvictMs = 60000;               // 60s idle eviction
  static constexpr size_t kDefaultMaxMemoryBytes = 512 * 1024 * 1024;  // 512MB RSS

  WorkerGuard();
  ~WorkerGuard();

  WorkerGuard(const WorkerGuard &) = delete;
  WorkerGuard &operator=(const WorkerGuard &) = delete;

  /**
   * @brief Initialize worker pool.
   *
   * @param worker_bin_path Absolute path to soeasy-worker binary, or empty to auto-resolve from executable directory
   * @param worker_count Number of worker processes to spawn
   * @param timeout_ms Max execution time allowed per document before SIGKILL
   * @return true on success
   */
  bool Init(const std::string &worker_bin_path, size_t worker_count = 2, uint32_t timeout_ms = kDefaultTimeoutMs);

  /**
   * @brief Terminate all workers, stop watchdog thread, and close RPC connections.
   */
  void Shutdown();

  /**
   * @brief Synchronously execute a document parsing task on an available sandbox worker.
   *
   * @param task Document parsing request
   * @param out_result Receives execution status, extracted size, and text
   * @return true if execution completed (even if status is timeout/crash/failed)
   */
  bool ExecuteTask(const ParseTask &task, ParseTaskResult *out_result);

  size_t worker_count() const;
  size_t active_worker_count() const;
  void SetWorkerCount(size_t new_count);
  uint32_t timeout_ms() const { return timeout_ms_.load(std::memory_order_relaxed); }
  void set_timeout_ms(uint32_t ms) {
    timeout_ms_.store(ms > 0 ? ms : kDefaultTimeoutMs, std::memory_order_relaxed);
  }
  uint32_t idle_evict_ms() const { return idle_evict_ms_.load(std::memory_order_relaxed); }
  void set_idle_evict_ms(uint32_t ms) { idle_evict_ms_.store(ms, std::memory_order_relaxed); }

  void SetLoggingOptions(const WorkerLoggingOptions &options);
  void SetLoggingOptions(const std::string &level, bool enable_console, bool enable_sync,
                         bool doctotext_logging = false, const std::string &doctotext_log_path = "",
                         bool embedded_image_ocr = false, const std::string &worker_log_dir = "");

 private:
  void ShutdownLocked();
  bool SpawnWorkerProcess(size_t index, DetachedWorkerResources *out_resources, WorkerPid *out_pid,
                          const std::vector<std::string> &args_template, bool log_as_info);
  static void DrainWorkerStdio(WorkerGuard *guard, FILE *stream, size_t index, bool log_as_info);
  void ClearWorkerStdio(size_t index);
  void RecordWorkerStdio(size_t index, const std::string &line);
  std::string GetRecentWorkerStdio(size_t index) const;
  std::vector<std::string> BuildWorkerArgs(const std::string &ipc_param, size_t index) const;
  void WatchdogLoop();

  std::string worker_bin_path_;
  std::atomic<uint32_t> timeout_ms_{kDefaultTimeoutMs};
  std::atomic<uint32_t> idle_evict_ms_{kDefaultIdleEvictMs};
  std::string log_level_ = "info";
  bool log_console_ = false;
  bool log_sync_ = false;
  bool doctotext_logging_ = false;
  std::string doctotext_log_path_;
  bool embedded_image_ocr_ = false;
  std::string worker_log_dir_ = "logs";

  std::mutex lifecycle_mutex_;
  uint64_t pool_generation_ = 0;
  size_t target_worker_count_ = 0;
  std::vector<std::unique_ptr<WorkerInstance>> workers_;
  mutable std::mutex stdio_mutex_;
  std::vector<std::deque<std::string>> recent_stdio_;
  mutable std::mutex pool_mutex_;
  std::condition_variable worker_available_cv_;

  std::atomic<bool> is_running_{false};
  std::mutex watchdog_mutex_;
  std::condition_variable watchdog_cv_;
  std::thread watchdog_thread_;
#ifdef _WIN32
  HANDLE job_handle_ = nullptr;
#endif
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_WORKER_GUARD_H_
