#ifndef SOEASY_SRC_SERVICE_WORKER_GUARD_H_
#define SOEASY_SRC_SERVICE_WORKER_GUARD_H_

#include <sys/types.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/shm_pool.h"
#include "subprocess.h"

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
  std::string spill_file_path;  // Set when text exceeds slot capacity and spilled to temp file
  std::string extracted_text;   // Streamed directly over Cap'n Proto IPC
  std::vector<std::string> stream_chunks;
  double elapsed_ms = 0.0;
};

enum class WorkerState { kIdle = 0, kBusy = 1, kKilledByWatchdog = 2, kCrashed = 3 };

struct SubprocessDeleter {
  void operator()(subprocess_s *proc) const noexcept {
    if (!proc) {
      return;
    }
#if defined(_WIN32)
    if (proc->hProcess) {
      if (subprocess_alive(proc)) {
        subprocess_terminate(proc);
      }
      int exit_code = 0;
      subprocess_join(proc, &exit_code);
    }
#else
    if (proc->child > 0 || proc->alive) {
      if (subprocess_alive(proc)) {
        subprocess_terminate(proc);
      }
      int exit_code = 0;
      subprocess_join(proc, &exit_code);
    }
#endif
    subprocess_destroy(proc);
    delete proc;
  }
};

using SubprocessHandle = std::unique_ptr<subprocess_s, SubprocessDeleter>;

inline SubprocessHandle MakeSubprocess() {
  return SubprocessHandle(new subprocess_s{}, SubprocessDeleter{});
}

struct WorkerInstance {
  SubprocessHandle process;
  std::thread stdio_drain_thread;
  pid_t pid = 0;
#ifdef _WIN32
  HANDLE pipe_handle = nullptr;
#else
  int socket_fd = -1;
#endif
  WorkerState state = WorkerState::kIdle;
  int64_t current_doc_id = 0;
  uint32_t current_slot_id = 0;
  uint32_t current_timeout_ms = 15000;
  std::chrono::steady_clock::time_point task_start_time;
  std::chrono::steady_clock::time_point idle_start_time;
  uint64_t total_tasks_processed = 0;

  WorkerInstance() = default;
  ~WorkerInstance() { Reset(); }
  WorkerInstance(WorkerInstance &&) noexcept = default;
  WorkerInstance &operator=(WorkerInstance &&) noexcept = default;
  WorkerInstance(const WorkerInstance &) = delete;
  WorkerInstance &operator=(const WorkerInstance &) = delete;

  bool is_busy() const { return state == WorkerState::kBusy; }

  void CloseTransport() noexcept;
  void Reset(WorkerState next_state = WorkerState::kIdle) noexcept;
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
  static constexpr uint32_t kDefaultTimeoutMs = 15000;                 // 15s default
  static constexpr uint32_t kDefaultStallTimeoutMs = 5000;             // 5s stall heartbeat
  static constexpr uint32_t kDefaultIdleEvictMs = 60000;               // 60s idle eviction
  static constexpr size_t kDefaultMaxMemoryBytes = 512 * 1024 * 1024;  // 512MB RSS

  WorkerGuard();
  ~WorkerGuard();

  // Disallow copy and assign
  WorkerGuard(const WorkerGuard &) = delete;
  WorkerGuard &operator=(const WorkerGuard &) = delete;

  /**
   * @brief Ensure orphan spill temp files from prior crashed runs are swept and removed.
   */
  static void CleanupOrphanSpillFiles();

  /**
   * @brief Canonical directory path for temporary spilled text files.
   */
  static std::string GetSpillDirectory();

  /**
   * @brief Initialize worker pool and shared memory pool.
   *
   * @param worker_bin_path Absolute path to soeasy-worker binary, or empty to auto-resolve from executable directory
   * @param worker_count Number of worker processes to spawn
   * @param timeout_ms Max execution time allowed per document before SIGKILL
   * @param shm_slot_count Number of SHM slots
   * @param shm_slot_capacity Capacity per slot in bytes
   * @return true on success
   */
  bool Init(const std::string &worker_bin_path, size_t worker_count = 2, uint32_t timeout_ms = kDefaultTimeoutMs,
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
  bool ExecuteTask(const ParseTask &task, ParseTaskResult *out_result);

  /**
   * @brief Access shared memory slot pool for reading zero-copy extracted text.
   */
  common::ShmSlotPool *shm_pool() { return &shm_pool_; }

  /**
   * @brief Safely release an allocated slot and notify any waiting worker threads.
   */
  void ReleaseSlot(uint32_t slot_id);

  size_t worker_count() const { return workers_.size(); }
  size_t active_worker_count() const;
  uint32_t timeout_ms() const { return timeout_ms_; }
  void set_timeout_ms(uint32_t ms) { timeout_ms_ = ms; }
  uint32_t idle_evict_ms() const { return idle_evict_ms_; }
  void set_idle_evict_ms(uint32_t ms) { idle_evict_ms_ = ms; }

  void SetLoggingOptions(const WorkerLoggingOptions &options);

  void SetLoggingOptions(const std::string &level, bool enable_console, bool enable_sync,
                         bool doctotext_logging = false, const std::string &doctotext_log_path = "",
                         bool embedded_image_ocr = false, const std::string &worker_log_dir = "");

 private:
  bool SpawnWorker(size_t index);
#ifdef _WIN32
  bool SpawnWorkerWindows(size_t index);
#else
  bool SpawnWorkerPosix(size_t index);
#endif
  void FinalizeSpawnedWorker(size_t index, SubprocessHandle proc, pid_t pid);
  void FinalizeBusyWorkerOnFailure(size_t worker_idx, uint32_t slot_id, TaskStatus status, const std::string &reason);
  static void DrainWorkerStdio(WorkerGuard *guard, FILE *stream, size_t index, bool log_as_info);
  void RecordWorkerStdio(size_t index, const std::string &line);
  std::string GetRecentWorkerStdio(size_t index) const;
  std::vector<std::string> BuildWorkerArgs(const std::string &ipc_param, size_t index) const;
  void KillWorker(size_t index, TaskStatus status, const std::string &reason);
  void WatchdogLoop();
  static size_t GetProcessRssBytes(pid_t pid);

  std::string worker_bin_path_;
  uint32_t timeout_ms_ = kDefaultTimeoutMs;
  uint32_t idle_evict_ms_ = kDefaultIdleEvictMs;
  std::string log_level_ = "info";
  bool log_console_ = false;
  bool log_sync_ = false;
  bool doctotext_logging_ = false;
  std::string doctotext_log_path_;
  bool embedded_image_ocr_ = false;
  std::string worker_log_dir_ = "logs";
  common::ShmSlotPool shm_pool_;

  std::vector<WorkerInstance> workers_;
  mutable std::mutex stdio_mutex_;
  std::vector<std::deque<std::string>> recent_stdio_;
  mutable std::mutex pool_mutex_;
  std::condition_variable worker_available_cv_;

  std::atomic<bool> is_running_{false};
  std::thread watchdog_thread_;
  std::mutex watchdog_mutex_;
  std::condition_variable watchdog_cv_;
#ifdef _WIN32
  HANDLE job_handle_ = nullptr;
#endif
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_WORKER_GUARD_H_
