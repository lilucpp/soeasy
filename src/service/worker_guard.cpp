#include "service/worker_guard.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#endif

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

#include "common/logger.h"
#include "common/shm_pool.h"
#include "soeasy_ipc.capnp.h"
#include "worker/l2_office_parser.h"
#include "worker/worker_loop.h"

namespace soeasy {
namespace service {

WorkerGuard::WorkerGuard() = default;

WorkerGuard::~WorkerGuard() {
  Shutdown();
}

size_t WorkerGuard::GetProcessRssBytes(pid_t pid) {
#ifdef _WIN32
  HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!hProcess) return 0;
  PROCESS_MEMORY_COUNTERS pmc;
  size_t bytes = 0;
  if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
    bytes = pmc.WorkingSetSize;
  }
  CloseHandle(hProcess);
  return bytes;
#else
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
#endif
}

bool WorkerGuard::Init(const std::string& worker_bin_path, size_t worker_count,
                       uint32_t timeout_ms, size_t shm_slot_count,
                       size_t shm_slot_capacity) {
  Shutdown();

  worker_bin_path_ = worker_bin_path;
  timeout_ms_ = timeout_ms;

#ifdef _WIN32
  job_handle_ = CreateJobObjectA(NULL, NULL);
  if (job_handle_) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    ZeroMemory(&jeli, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_handle_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
  }
  std::string shm_name = "Local\\soeasy_shm_" + std::to_string(GetCurrentProcessId()) + "_" +
                         std::to_string(rand() % 1000000);
#else
  std::string shm_name = "/soeasy_shm_" + std::to_string(getpid()) + "_" +
                         std::to_string(rand() % 1000000);
#endif
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

#ifdef _WIN32
  std::string pipe_name = "\\\\.\\pipe\\soeasy_ipc_" + std::to_string(GetCurrentProcessId()) +
                          "_" + std::to_string(index) + "_" + std::to_string(rand() % 1000000);
  HANDLE hPipe = CreateNamedPipeA(
      pipe_name.c_str(),
      PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1,
      1024 * 1024,
      1024 * 1024,
      5000,
      NULL);
  if (hPipe == INVALID_HANDLE_VALUE) {
    LOG_ERROR("[WorkerGuard] Failed to create named pipe for worker {}", index);
    return false;
  }

  std::string target_bin = worker_bin_path_;
  if (target_bin.empty()) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    target_bin = exe_path;
  }

  std::string cmd = "\"" + target_bin + "\" " +
                    "--pipe-name=" + pipe_name + " " +
                    "--shm-name=" + shm_pool_.shm_name() + " " +
                    "--slot-count=" + std::to_string(shm_pool_.slot_count()) + " " +
                    "--slot-capacity=" + std::to_string(shm_pool_.slot_capacity()) + " " +
                    "--log-level=" + log_level_ + " " +
                    (log_console_ ? "--log-console " : "--no-log-console ") +
                    (log_sync_ ? "--log-sync " : "--no-log-sync ") +
                    (doctotext_logging_ ? "--doctotext-logging " : "--no-doctotext-logging ") +
                    "--doctotext-log-path=" + doctotext_log_path_ + " " +
                    (embedded_image_ocr_ ? "--embedded-image-ocr" : "--no-embedded-image-ocr");

  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  if (!CreateProcessA(NULL, const_cast<char*>(cmd.c_str()), NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    LOG_ERROR("[WorkerGuard] CreateProcess failed for worker {}: {}", index, GetLastError());
    CloseHandle(hPipe);
    return false;
  }
  CloseHandle(pi.hThread);

  if (job_handle_) {
    AssignProcessToJobObject(job_handle_, pi.hProcess);
  }

  BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
  if (!connected) {
    LOG_ERROR("[WorkerGuard] ConnectNamedPipe failed for worker {}: {}", index, GetLastError());
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(hPipe);
    return false;
  }

  WorkerInstance& inst = workers_[index];
  inst.pid = pi.dwProcessId;
  inst.process_handle = pi.hProcess;
  inst.pipe_handle = hPipe;
  inst.state = WorkerState::kIdle;
  inst.current_doc_id = 0;
  inst.current_slot_id = 0;
  inst.total_tasks_processed = 0;
  return true;
#else
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

    if (!log_console_) {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
    }

    if (worker_bin_path_.empty()) {
      // In-process child execution mode
      common::Logger::SetLevel(log_level_);
      common::Logger::SetConsoleEnabled(log_console_);
      common::Logger::SetSyncMode(log_sync_);
      worker::L2OfficeParser::SetLogging(doctotext_logging_, doctotext_log_path_);
      worker::L2OfficeParser::SetEmbeddedImageOcr(embedded_image_ocr_);
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
      std::string level_arg = "--log-level=" + log_level_;
      std::string console_arg = log_console_ ? "--log-console" : "--no-log-console";
      std::string sync_arg = log_sync_ ? "--log-sync" : "--no-log-sync";
      std::string dt_log_arg = doctotext_logging_ ? "--doctotext-logging" : "--no-doctotext-logging";
      std::string dt_path_arg = "--doctotext-log-path=" + doctotext_log_path_;
      std::string img_ocr_arg = embedded_image_ocr_ ? "--embedded-image-ocr" : "--no-embedded-image-ocr";

      char* const argv[] = {
          const_cast<char*>(worker_bin_path_.c_str()),
          const_cast<char*>(fd_arg.c_str()),
          const_cast<char*>(shm_arg.c_str()),
          const_cast<char*>(count_arg.c_str()),
          const_cast<char*>(cap_arg.c_str()),
          const_cast<char*>(level_arg.c_str()),
          const_cast<char*>(console_arg.c_str()),
          const_cast<char*>(sync_arg.c_str()),
          const_cast<char*>(dt_log_arg.c_str()),
          const_cast<char*>(dt_path_arg.c_str()),
          const_cast<char*>(img_ocr_arg.c_str()),
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
#endif
}

void WorkerGuard::KillWorker(size_t index, TaskStatus status,
                             const std::string& reason) {
  if (index >= workers_.size()) {
    return;
  }
  WorkerInstance& inst = workers_[index];
#ifdef _WIN32
  if (inst.process_handle) {
    TerminateProcess(inst.process_handle, 1);
    WaitForSingleObject(inst.process_handle, 1000);
    CloseHandle(inst.process_handle);
    inst.process_handle = nullptr;
  }
  if (inst.pipe_handle) {
    CloseHandle(inst.pipe_handle);
    inst.pipe_handle = nullptr;
  }
  inst.pid = 0;
#else
  if (inst.pid > 0) {
    kill(inst.pid, SIGKILL);
    waitpid(inst.pid, nullptr, 0);
  }
  if (inst.socket_fd >= 0) {
    close(inst.socket_fd);
    inst.socket_fd = -1;
  }
  inst.pid = -1;
#endif
  if (inst.is_busy()) {
    shm_pool_.ReleaseSlot(inst.current_slot_id);
  }
  inst.state = WorkerState::kIdle;

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
#ifdef _WIN32
      DWORD exit_code = 0;
      if (inst.process_handle && GetExitCodeProcess(inst.process_handle, &exit_code) &&
          exit_code != STILL_ACTIVE) {
        if (inst.process_handle) {
          CloseHandle(inst.process_handle);
          inst.process_handle = nullptr;
        }
        if (inst.pipe_handle) {
          CloseHandle(inst.pipe_handle);
          inst.pipe_handle = nullptr;
        }
        if (inst.is_busy()) {
          shm_pool_.ReleaseSlot(inst.current_slot_id);
        }
        inst.state = WorkerState::kIdle;
        inst.pid = 0;
        SpawnWorker(i);
        worker_available_cv_.notify_one();
        continue;
      }
#else
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
#endif

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
#ifdef _WIN32
          if (inst.process_handle) {
            TerminateProcess(inst.process_handle, 1);
            WaitForSingleObject(inst.process_handle, 1000);
            CloseHandle(inst.process_handle);
            inst.process_handle = nullptr;
          }
          if (inst.pipe_handle) {
            CloseHandle(inst.pipe_handle);
            inst.pipe_handle = nullptr;
          }
          inst.pid = 0;
#else
          if (inst.pid > 0) {
            kill(inst.pid, SIGKILL);
            waitpid(inst.pid, nullptr, 0);
            inst.pid = -1;
          }
          if (inst.socket_fd >= 0) {
            close(inst.socket_fd);
            inst.socket_fd = -1;
          }
#endif
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
#ifdef _WIN32
  HANDLE worker_pipe = nullptr;
#else
  int worker_fd = -1;
#endif

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
#ifdef _WIN32
        if (workers_[i].state == WorkerState::kIdle && workers_[i].pid > 0 &&
            workers_[i].pipe_handle != nullptr) {
#else
        if (workers_[i].state == WorkerState::kIdle && workers_[i].pid > 0 &&
            workers_[i].socket_fd >= 0) {
#endif
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
#ifdef _WIN32
    worker_pipe = worker.pipe_handle;
#else
    worker_fd = worker.socket_fd;
#endif
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

  // Send request over IPC channel
#ifdef _WIN32
  try {
    kj::HandleOutputStream out_stream(worker_pipe);
    capnp::writeMessage(out_stream, req_builder);
  } catch (...) {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    shm_pool_.ReleaseSlot(slot_id);
    KillWorker(assigned_worker_idx, TaskStatus::kFailed, "Pipe write failure");
    out_result->status = TaskStatus::kFailed;
    out_result->error_message = "Failed to send request to worker pipe";
    worker_available_cv_.notify_one();
    return true;
  }
#else
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
#endif

  // Read response with timeout checking
  bool success_response = false;
#ifdef _WIN32
  try {
    DWORD bytes_avail = 0;
    auto wait_start = std::chrono::steady_clock::now();
    uint32_t wait_timeout = task.timeout_ms > 0 ? task.timeout_ms : timeout_ms_;
    while (is_running_.load(std::memory_order_acquire)) {
      if (!PeekNamedPipe(worker_pipe, NULL, 0, NULL, &bytes_avail, NULL)) {
        break;
      }
      if (bytes_avail > 0) {
        break;
      }
      auto wait_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - wait_start).count();
      if (wait_elapsed >= wait_timeout) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (bytes_avail > 0) {
      kj::HandleInputStream in_stream(worker_pipe);
      capnp::InputStreamMessageReader resp_reader(in_stream);
      auto resp = resp_reader.getRoot<soeasy::ipc::ParseResult>();

      out_result->status = (resp.getStatusCode() == soeasy::ipc::ParseStatusCode::OK)
                               ? TaskStatus::kSuccess
                               : TaskStatus::kFailed;
      out_result->slot_id = slot_id;
      out_result->extracted_bytes = resp.getExtractedBytes();
      out_result->is_scanned_pdf = resp.getIsScannedPdf();
      out_result->error_message = resp.getMessage();
      success_response = true;
    }
  } catch (...) {
    success_response = false;
  }
#else
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
#endif

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
#ifdef _WIN32
    if (inst.process_handle) {
      TerminateProcess(inst.process_handle, 1);
      WaitForSingleObject(inst.process_handle, 1000);
      CloseHandle(inst.process_handle);
      inst.process_handle = nullptr;
    }
    if (inst.pipe_handle) {
      CloseHandle(inst.pipe_handle);
      inst.pipe_handle = nullptr;
    }
    inst.pid = 0;
#else
    if (inst.pid > 0) {
      kill(inst.pid, SIGKILL);
      waitpid(inst.pid, nullptr, 0);
      inst.pid = -1;
    }
    if (inst.socket_fd >= 0) {
      close(inst.socket_fd);
      inst.socket_fd = -1;
    }
#endif
  }
#ifdef _WIN32
  if (job_handle_) {
    CloseHandle(job_handle_);
    job_handle_ = nullptr;
  }
#endif
  workers_.clear();
  shm_pool_.Close();
}

void WorkerGuard::SetLoggingOptions(const std::string& level, bool enable_console,
                                    bool enable_sync, bool doctotext_logging,
                                    const std::string& doctotext_log_path,
                                    bool embedded_image_ocr) {
  std::lock_guard<std::mutex> lock(pool_mutex_);
  bool changed = (log_level_ != level || log_console_ != enable_console ||
                  log_sync_ != enable_sync ||
                  doctotext_logging_ != doctotext_logging ||
                  doctotext_log_path_ != doctotext_log_path ||
                  embedded_image_ocr_ != embedded_image_ocr);
  log_level_ = level;
  log_console_ = enable_console;
  log_sync_ = enable_sync;
  doctotext_logging_ = doctotext_logging;
  doctotext_log_path_ = doctotext_log_path;
  embedded_image_ocr_ = embedded_image_ocr;

  // In in-process mode, apply directly
  if (worker_bin_path_.empty()) {
    worker::L2OfficeParser::SetLogging(doctotext_logging_, doctotext_log_path_);
    worker::L2OfficeParser::SetEmbeddedImageOcr(embedded_image_ocr_);
  }

  if (changed) {
    // Respawn idle workers with new arguments
    for (size_t i = 0; i < workers_.size(); ++i) {
      if (!workers_[i].is_busy() && workers_[i].pid > 0) {
        KillWorker(i, TaskStatus::kSuccess, "Options changed");
      }
    }
  }
}

}  // namespace service
}  // namespace soeasy
