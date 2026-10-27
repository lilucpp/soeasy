#include "service/worker_guard.h"

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cstring>

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/io.h>

#include "common/shm_pool.h"
#include "soeasy_ipc.capnp.h"
#include "worker/worker_loop.h"

namespace soeasy {
namespace service {

WorkerGuard::WorkerGuard() = default;

WorkerGuard::~WorkerGuard() {
  Shutdown();
}

size_t WorkerGuard::GetProcessRssBytes(pid_t pid) {
  std::string statm_path = "/proc/" + std::to_string(pid) + "/statm";
  std::ifstream statm(statm_path);
  if (!statm.is_open()) {
    return 0;
  }
  uint64_t size_pages = 0;
  uint64_t resident_pages = 0;
  statm >> size_pages >> resident_pages;
  long page_size = sysconf(_SC_PAGESIZE);
  return resident_pages * (page_size > 0 ? page_size : 4096);
}

bool WorkerGuard::Init(const std::string& worker_bin_path, size_t worker_count,
                       uint32_t timeout_ms, size_t shm_slot_count,
                       size_t shm_slot_capacity) {
  Shutdown();

  worker_bin_path_ = worker_bin_path;
  timeout_ms_ = timeout_ms;

  std::string shm_name = "/soeasy_shm_" + std::to_string(getpid()) + "_" +
                         std::to_string(rand() % 1000000);
  if (!shm_pool_.Create(shm_name, shm_slot_count, shm_slot_capacity)) {
    return false;
  }

  is_running_.store(true, std::memory_order_release);
  workers_.resize(worker_count);

  for (size_t i = 0; i < worker_count; ++i) {
    if (!SpawnWorker(i)) {
      Shutdown();
      return false;
    }
  }

  watchdog_thread_ = std::thread(&WorkerGuard::WatchdogLoop, this);
  return true;
}

bool WorkerGuard::SpawnWorker(size_t index) {
  if (index >= workers_.size()) {
    return false;
  }

  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }

  if (pid == 0) {
    // Child process
    close(fds[0]);  // Close parent end
    // Ensure child dies when parent dies
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    if (worker_bin_path_.empty()) {
      // In-process child execution mode
      int ret = worker::RunWorkerLoop(fds[1], shm_pool_.shm_name(),
                                      shm_pool_.slot_count(),
                                      shm_pool_.slot_capacity());
      close(fds[1]);
      _exit(ret);
    } else {
      // Exec standalone binary mode
      std::string fd_arg = "--socket-fd=" + std::to_string(fds[1]);
      std::string shm_arg = "--shm-name=" + shm_pool_.shm_name();
      std::string count_arg =
          "--slot-count=" + std::to_string(shm_pool_.slot_count());
      std::string cap_arg =
          "--slot-capacity=" + std::to_string(shm_pool_.slot_capacity());

      char* const argv[] = {
          const_cast<char*>(worker_bin_path_.c_str()),
          const_cast<char*>(fd_arg.c_str()),
          const_cast<char*>(shm_arg.c_str()),
          const_cast<char*>(count_arg.c_str()),
          const_cast<char*>(cap_arg.c_str()),
          nullptr};
      execv(worker_bin_path_.c_str(), argv);
      _exit(127);
    }
  }

  // Parent process
  close(fds[1]);  // Close child end

  WorkerInstance& inst = workers_[index];
  inst.pid = pid;
  inst.socket_fd = fds[0];
  inst.state = WorkerState::kIdle;
  inst.current_doc_id = 0;
  inst.current_slot_id = 0;
  inst.total_tasks_processed = 0;
  return true;
}

void WorkerGuard::KillWorker(size_t index, TaskStatus status,
                             const std::string& reason) {
  if (index >= workers_.size()) {
    return;
  }
  WorkerInstance& inst = workers_[index];
  if (inst.pid > 0) {
    kill(inst.pid, SIGKILL);
    waitpid(inst.pid, nullptr, 0);
  }
  if (inst.socket_fd >= 0) {
    close(inst.socket_fd);
    inst.socket_fd = -1;
  }
  if (inst.is_busy()) {
    shm_pool_.ReleaseSlot(inst.current_slot_id);
  }
  inst.state = WorkerState::kIdle;
  inst.pid = -1;

  // Respawn clean replacement worker
  SpawnWorker(index);
}

void WorkerGuard::WatchdogLoop() {
  while (is_running_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::unique_lock<std::mutex> lock(pool_mutex_);
    auto now = std::chrono::steady_clock::now();

    for (size_t i = 0; i < workers_.size(); ++i) {
      WorkerInstance& inst = workers_[i];
      if (inst.pid <= 0) {
        continue;
      }

      // 1. Check if child crashed (exited unexpectedly)
      int wstatus = 0;
      pid_t res = waitpid(inst.pid, &wstatus, WNOHANG);
      if (res > 0) {
        // Child died
        if (inst.socket_fd >= 0) {
          close(inst.socket_fd);
          inst.socket_fd = -1;
        }
        if (inst.is_busy()) {
          shm_pool_.ReleaseSlot(inst.current_slot_id);
        }
        inst.state = WorkerState::kIdle;
        inst.pid = -1;
        SpawnWorker(i);
        worker_available_cv_.notify_one();
        continue;
      }

      // 2. Check for task timeout or stall heartbeat
      if (inst.state == WorkerState::kBusy) {
        uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - inst.task_start_time)
                           .count();

        uint64_t last_hb_ms = shm_pool_.GetSlotLastHeartbeat(inst.current_slot_id);
        bool is_stalled = (last_hb_ms > 0 && (now_ms > last_hb_ms) &&
                           (now_ms - last_hb_ms > kDefaultStallTimeoutMs));
        bool is_overtime = (elapsed > inst.current_timeout_ms);

        if (is_stalled || is_overtime) {
          inst.state = WorkerState::kKilledByWatchdog;
          shm_pool_.ReleaseSlot(inst.current_slot_id);
          if (inst.pid > 0) {
            kill(inst.pid, SIGKILL);
            waitpid(inst.pid, nullptr, 0);
            inst.pid = -1;
          }
          if (inst.socket_fd >= 0) {
            close(inst.socket_fd);
            inst.socket_fd = -1;
          }
          // Do not spawn here, let ExecuteTask unblock and safely respawn
          continue;
        }
      }

      // 3. Check memory limit when idle
      if (inst.state == WorkerState::kIdle) {
        size_t rss = GetProcessRssBytes(inst.pid);
        if (rss > kDefaultMaxMemoryBytes) {
          KillWorker(i, TaskStatus::kSuccess, "Memory limit exceeded; recycling");
          worker_available_cv_.notify_one();
        }
      }
    }
  }
}

bool WorkerGuard::ExecuteTask(const ParseTask& task,
                              ParseTaskResult* out_result) {
  if (!out_result) {
    return false;
  }
  out_result->doc_id = task.doc_id;
  out_result->status = TaskStatus::kFailed;
  out_result->slot_id = 0;
  out_result->extracted_bytes = 0;

  uint32_t slot_id = 0;
  uint8_t* slot_data = nullptr;

  size_t assigned_worker_idx = 0;
  int worker_fd = -1;

  auto start_time = std::chrono::steady_clock::now();

  {
    std::unique_lock<std::mutex> lock(pool_mutex_);

    // Wait until both an idle worker AND an available SHM slot exist
    worker_available_cv_.wait(lock, [this, &assigned_worker_idx, &slot_id,
                                     &slot_data]() {
      if (!is_running_.load(std::memory_order_acquire)) {
        return true;
      }
      for (size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i].state == WorkerState::kIdle && workers_[i].pid > 0 &&
            workers_[i].socket_fd >= 0) {
          if (shm_pool_.AllocateSlot(&slot_id, &slot_data)) {
            assigned_worker_idx = i;
            return true;
          }
        }
      }
      return false;
    });

    if (!is_running_.load(std::memory_order_acquire)) {
      return false;
    }

    WorkerInstance& worker = workers_[assigned_worker_idx];
    worker.state = WorkerState::kBusy;
    worker.current_doc_id = task.doc_id;
    worker.current_slot_id = slot_id;
    worker.current_timeout_ms = task.timeout_ms > 0 ? task.timeout_ms : timeout_ms_;
    worker.task_start_time = start_time;
    worker_fd = worker.socket_fd;
  }

  // Build Cap'n Proto request
  capnp::MallocMessageBuilder req_builder;
  auto req = req_builder.initRoot<soeasy::ipc::ParseRequest>();
  req.setDocId(task.doc_id);
  req.setFilePath(task.file_path);
  req.setExt(task.ext);
  req.setLevel(soeasy::ipc::ParseLevel::L1_TEXT);
  auto target_slot = req.initTargetSlot();
  target_slot.setShmName(shm_pool_.shm_name());
  target_slot.setSlotId(slot_id);
  target_slot.setDataSize(shm_pool_.slot_capacity());

  // Send request over Unix domain socket
  try {
    capnp::writeMessageToFd(worker_fd, req_builder);
  } catch (...) {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    shm_pool_.ReleaseSlot(slot_id);
    KillWorker(assigned_worker_idx, TaskStatus::kFailed, "Socket write failure");
    out_result->status = TaskStatus::kFailed;
    out_result->error_message = "Failed to send request to worker socket";
    worker_available_cv_.notify_one();
    return true;
  }

  // Read response with timeout checking
  bool success_response = false;
  try {
    capnp::StreamFdMessageReader resp_reader(worker_fd);
    auto resp = resp_reader.getRoot<soeasy::ipc::ParseResult>();

    out_result->status = (resp.getStatusCode() == soeasy::ipc::ParseStatusCode::OK)
                             ? TaskStatus::kSuccess
                             : TaskStatus::kFailed;
    out_result->slot_id = slot_id;
    out_result->extracted_bytes = resp.getExtractedBytes();
    out_result->is_scanned_pdf = resp.getIsScannedPdf();
    out_result->error_message = resp.getMessage();
    success_response = true;
  } catch (...) {
    // If reading failed, either the worker crashed or the watchdog killed it for timeout
    success_response = false;
  }

  auto finish_time = std::chrono::steady_clock::now();
  out_result->elapsed_ms =
      std::chrono::duration<double, std::milli>(finish_time - start_time)
          .count();

  {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    WorkerInstance& worker = workers_[assigned_worker_idx];

    if (worker.state == WorkerState::kKilledByWatchdog) {
      // Watchdog already cleaned up the slot and terminated the worker.
      // Avoid double-free and mis-killing new workers.
      out_result->status = TaskStatus::kTimeout;
      out_result->error_message = "Task timed out or stalled (killed by watchdog)";
      worker.state = WorkerState::kIdle;
      SpawnWorker(assigned_worker_idx);
    } else if (success_response) {
      worker.state = WorkerState::kIdle;
      worker.total_tasks_processed++;
      if (out_result->status != TaskStatus::kSuccess) {
        // Task returned FAILED from worker: release the slot immediately
        shm_pool_.ReleaseSlot(slot_id);
        out_result->slot_id = 0;
      }
    } else {
      out_result->status = TaskStatus::kCrashed;
      out_result->error_message =
          "Worker crashed (SIGSEGV/abort) during file parsing";
      shm_pool_.ReleaseSlot(slot_id);
      out_result->slot_id = 0;
      KillWorker(assigned_worker_idx, out_result->status, out_result->error_message);
    }
    worker_available_cv_.notify_one();
  }

  return true;
}

void WorkerGuard::Shutdown() {
  if (!is_running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  worker_available_cv_.notify_all();
  if (watchdog_thread_.joinable()) {
    watchdog_thread_.join();
  }

  std::unique_lock<std::mutex> lock(pool_mutex_);
  for (auto& inst : workers_) {
    if (inst.pid > 0) {
      kill(inst.pid, SIGKILL);
      waitpid(inst.pid, nullptr, 0);
      inst.pid = -1;
    }
    if (inst.socket_fd >= 0) {
      close(inst.socket_fd);
      inst.socket_fd = -1;
    }
  }
  workers_.clear();
  shm_pool_.Close();
}

}  // namespace service
}  // namespace soeasy
