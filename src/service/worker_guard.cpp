#include "service/worker_guard.h"

#ifdef _WIN32
#include <psapi.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/io.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "common/logger.h"
#include "common/shm_pool.h"
#include "soeasy_ipc.capnp.h"

namespace soeasy {
namespace service {

namespace {

uint64_t NextUniqueIpcTag() {
  static std::atomic<uint64_t> seq{1};
  auto now_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
  return (static_cast<uint64_t>(now_us) ^ (seq.fetch_add(1, std::memory_order_relaxed) * 1000003ULL)) % 1000000000ULL;
}

std::vector<const char *> ToArgv(const std::vector<std::string> &args) {
  std::vector<const char *> cmd;
  cmd.reserve(args.size() + 1);
  for (const auto &arg : args) {
    cmd.push_back(arg.c_str());
  }
  cmd.push_back(nullptr);
  return cmd;
}

std::string ResolveWorkerBinary(const std::string &hint_path) {
  std::error_code ec;
  if (!hint_path.empty() && std::filesystem::exists(hint_path, ec)) {
    return std::filesystem::canonical(hint_path, ec).string();
  }

  // 1. Try directory containing current executable
#ifdef _WIN32
  char exe_buf[MAX_PATH];
  if (GetModuleFileNameA(NULL, exe_buf, MAX_PATH) > 0) {
    std::filesystem::path dir = std::filesystem::path(exe_buf).parent_path();
    std::filesystem::path target = dir / "soeasy-worker.exe";
    if (std::filesystem::exists(target, ec)) {
      return std::filesystem::canonical(target, ec).string();
    }
  }
#elif defined(__linux__)
  char exe_buf[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
  if (len > 0) {
    exe_buf[len] = '\0';
    std::filesystem::path dir = std::filesystem::path(exe_buf).parent_path();
    std::filesystem::path target = dir / "soeasy-worker";
    if (std::filesystem::exists(target, ec)) {
      return std::filesystem::canonical(target, ec).string();
    }
  }
#endif

  return "";
}

std::string DeriveWorkerDocToTextLogPath(const std::string &configured_path, const std::string &log_dir, size_t index) {
  const std::string suffix = "_worker_" + std::to_string(index);
  if (configured_path.empty()) {
    return (std::filesystem::path(log_dir) / ("doctotext" + suffix + ".log")).generic_string();
  }
  const std::filesystem::path p(configured_path);
  if (!p.has_filename()) {
    return (p / ("doctotext" + suffix + ".log")).generic_string();
  }
  return (p.parent_path() / (p.stem().string() + suffix + p.extension().string())).generic_string();
}

}  // namespace

void WorkerInstance::CloseTransport() noexcept {
#ifdef _WIN32
  if (pipe_handle) {
    CloseHandle(pipe_handle);
    pipe_handle = nullptr;
  }
#else
  if (socket_fd >= 0) {
    shutdown(socket_fd, SHUT_RDWR);
    close(socket_fd);
    socket_fd = -1;
  }
#endif
}

void WorkerInstance::Reset(WorkerState next_state) noexcept {
  CloseTransport();
  // Terminate and join the child before joining the stdio drain thread so its pipe write end closes cleanly.
  if (process) {
    if (subprocess_alive(process.get())) {
      subprocess_terminate(process.get());
    }
    int exit_code = 0;
    subprocess_join(process.get(), &exit_code);
  }
  if (stdio_drain_thread.joinable()) {
    stdio_drain_thread.join();
  }
  process.reset();
  pid = 0;
  state = next_state;
  current_doc_id = 0;
  current_slot_id = 0;
}

WorkerGuard::WorkerGuard() = default;

WorkerGuard::~WorkerGuard() {
  Shutdown();
}

size_t WorkerGuard::active_worker_count() const {
  std::lock_guard<std::mutex> lock(pool_mutex_);
  size_t count = 0;
  for (const auto &w : workers_) {
    if (w.pid > 0) count++;
  }
  return count;
}

void WorkerGuard::ReleaseSlot(uint32_t slot_id) {
  shm_pool_.ReleaseSlot(slot_id);
  std::lock_guard<std::mutex> lock(pool_mutex_);
  worker_available_cv_.notify_all();
}

std::string WorkerGuard::GetSpillDirectory() {
  std::error_code ec;
  std::filesystem::path p = std::filesystem::temp_directory_path(ec) / "soeasy_spill";
  std::filesystem::create_directories(p, ec);
  return p.string();
}

void WorkerGuard::CleanupOrphanSpillFiles() {
  std::error_code ec;
  std::filesystem::path dir = GetSpillDirectory();
  if (!std::filesystem::exists(dir, ec)) return;

  size_t cleaned = 0;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    if (entry.is_regular_file(ec)) {
      std::string fname = entry.path().filename().string();
      if (fname.rfind("soeasy_spill_", 0) == 0 && fname.find(".tmp") != std::string::npos) {
        std::filesystem::remove(entry.path(), ec);
        cleaned++;
      }
    }
  }
  if (cleaned > 0) {
    LOG_INFO("[WorkerGuard] Swept and cleaned {} orphan spill files from prior runs.", cleaned);
  }
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

bool WorkerGuard::Init(const std::string &worker_bin_path, size_t worker_count, uint32_t timeout_ms,
                       size_t shm_slot_count, size_t shm_slot_capacity) {
  Shutdown();

#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  // Sweep orphan spill files from prior crashes or reboots
  CleanupOrphanSpillFiles();

  worker_bin_path_ = ResolveWorkerBinary(worker_bin_path);
  if (worker_bin_path_.empty()) {
    LOG_ERROR(
        "[WorkerGuard] Failed to locate soeasy-worker binary (hint: '{}'). Multithreaded in-process fork is "
        "prohibited.",
        worker_bin_path);
    return false;
  }
  timeout_ms_ = timeout_ms;

  // Auto-calculate dynamic slot count: worker_count + 1
  if (shm_slot_count == 0 || shm_slot_count == common::ShmSlotPool::kDefaultSlotCount) {
    shm_slot_count = worker_count + 1;
  }
  if (shm_slot_capacity == 0) {
    shm_slot_capacity = common::ShmSlotPool::kDefaultSlotCapacity;
  }

#ifdef _WIN32
  job_handle_ = CreateJobObjectA(NULL, NULL);
  if (job_handle_) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    ZeroMemory(&jeli, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_handle_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
  }
  std::string shm_name =
      "Local\\soeasy_shm_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(NextUniqueIpcTag());
#else
  std::string shm_name = "/soeasy_shm_" + std::to_string(getpid()) + "_" + std::to_string(NextUniqueIpcTag());
#endif
  if (!shm_pool_.Create(shm_name, shm_slot_count, shm_slot_capacity)) {
    return false;
  }

  is_running_.store(true, std::memory_order_release);
  workers_.resize(worker_count);
  {
    std::lock_guard<std::mutex> lock(stdio_mutex_);
    recent_stdio_.resize(worker_count);
  }

  auto now = std::chrono::steady_clock::now();
  for (size_t i = 0; i < worker_count; ++i) {
    workers_[i].pid = 0;
    workers_[i].state = WorkerState::kIdle;
    workers_[i].idle_start_time = now;
  }

  watchdog_thread_ = std::thread(&WorkerGuard::WatchdogLoop, this);
  return true;
}

std::vector<std::string> WorkerGuard::BuildWorkerArgs(const std::string &ipc_param, size_t index) const {
  const std::string log_dir = worker_log_dir_.empty() ? "logs" : worker_log_dir_;
  const std::string worker_log_file =
      (std::filesystem::path(log_dir) / ("soeasy_worker_" + std::to_string(index) + ".log")).generic_string();
  const std::string worker_dt_log_file = DeriveWorkerDocToTextLogPath(doctotext_log_path_, log_dir, index);
  return {worker_bin_path_,
          ipc_param,
          "--shm-name=" + shm_pool_.shm_name(),
          "--slot-count=" + std::to_string(shm_pool_.slot_count()),
          "--slot-capacity=" + std::to_string(shm_pool_.slot_capacity()),
          "--log-file=" + worker_log_file,
          "--log-level=" + log_level_,
          log_console_ ? "--log-console" : "--no-log-console",
          log_sync_ ? "--log-sync" : "--no-log-sync",
          doctotext_logging_ ? "--doctotext-logging" : "--no-doctotext-logging",
          "--doctotext-log-path=" + worker_dt_log_file,
          embedded_image_ocr_ ? "--embedded-image-ocr" : "--no-embedded-image-ocr"};
}

void WorkerGuard::RecordWorkerStdio(size_t index, const std::string &line) {
  std::lock_guard<std::mutex> lock(stdio_mutex_);
  if (index >= recent_stdio_.size()) {
    recent_stdio_.resize(index + 1);
  }
  if (recent_stdio_[index].size() >= 30) {
    recent_stdio_[index].pop_front();
  }
  recent_stdio_[index].push_back(line);
}

std::string WorkerGuard::GetRecentWorkerStdio(size_t index) const {
  std::lock_guard<std::mutex> lock(stdio_mutex_);
  if (index >= recent_stdio_.size()) return {};
  std::string out;
  for (const auto &l : recent_stdio_[index]) {
    if (!out.empty()) out += "\n  ";
    out += l;
  }
  return out;
}

void WorkerGuard::DrainWorkerStdio(WorkerGuard *guard, FILE *stream, size_t index, bool log_as_info) {
  if (!stream) return;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), stream) != nullptr) {
    size_t len = std::strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
      buf[--len] = '\0';
    }
    if (len == 0) continue;
    if (guard) {
      guard->RecordWorkerStdio(index, buf);
    }
    if (log_as_info) {
      LOG_INFO("[Worker {} stdio] {}", index, buf);
    } else {
      LOG_DEBUG("[Worker {} stdio] {}", index, buf);
    }
  }
}

void WorkerGuard::FinalizeSpawnedWorker(size_t index, SubprocessHandle proc, pid_t pid) {
  WorkerInstance &inst = workers_[index];
  inst.process = std::move(proc);
  inst.pid = pid;
  inst.state = WorkerState::kIdle;
  inst.idle_start_time = std::chrono::steady_clock::now();
  inst.current_doc_id = 0;
  inst.current_slot_id = 0;
  inst.total_tasks_processed = 0;
  if (inst.stdio_drain_thread.joinable()) {
    inst.stdio_drain_thread.join();
  }
  if (inst.process && inst.process->stdout_file) {
    inst.stdio_drain_thread =
        std::thread(&WorkerGuard::DrainWorkerStdio, this, inst.process->stdout_file, index, log_console_);
  }
}

#ifdef _WIN32
bool WorkerGuard::SpawnWorkerWindows(size_t index) {
  std::string pipe_name = "\\\\.\\pipe\\soeasy_ipc_" + std::to_string(GetCurrentProcessId()) + "_" +
                          std::to_string(index) + "_" + std::to_string(NextUniqueIpcTag());
  HANDLE hPipe =
      CreateNamedPipeA(pipe_name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                       1024 * 1024, 1024 * 1024, 5000, NULL);
  if (hPipe == INVALID_HANDLE_VALUE) {
    LOG_ERROR("[WorkerGuard] Failed to create named pipe for worker {}", index);
    return false;
  }

  std::vector<std::string> args = BuildWorkerArgs("--pipe-name=" + pipe_name, index);
  auto cmd = ToArgv(args);

  int options =
      subprocess_option_inherit_environment | subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
  auto proc = MakeSubprocess();
  int r = subprocess_create(cmd.data(), options, proc.get());
  if (r != 0) {
    LOG_ERROR("[WorkerGuard] subprocess_create failed to start worker {}: error={}", index, r);
    CloseHandle(hPipe);
    return false;
  }

  pid_t pid = static_cast<pid_t>(GetProcessId(proc->hProcess));

  if (job_handle_ && proc->hProcess) {
    AssignProcessToJobObject(job_handle_, proc->hProcess);
  }

  BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
  if (!connected) {
    LOG_ERROR("[WorkerGuard] ConnectNamedPipe failed for worker {}: {}", index, GetLastError());
    CloseHandle(hPipe);
    return false;
  }

  workers_[index].pipe_handle = hPipe;
  FinalizeSpawnedWorker(index, std::move(proc), pid);
  return true;
}
#else
bool WorkerGuard::SpawnWorkerPosix(size_t index) {
  int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    LOG_ERROR("[WorkerGuard] Failed to create socket for worker {}", index);
    return false;
  }

  std::string sock_name =
      "soeasy_ipc_" + std::to_string(getpid()) + "_" + std::to_string(index) + "_" + std::to_string(NextUniqueIpcTag());
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  addr.sun_path[0] = '\0';
  std::memcpy(addr.sun_path + 1, sock_name.c_str(), sock_name.size());
  socklen_t addr_len = sizeof(sa_family_t) + 1 + sock_name.size();

  if (bind(listen_fd, reinterpret_cast<struct sockaddr *>(&addr), addr_len) != 0) {
    LOG_ERROR("[WorkerGuard] Failed to bind socket for worker {}: {}", index, strerror(errno));
    close(listen_fd);
    return false;
  }

  if (listen(listen_fd, 1) != 0) {
    LOG_ERROR("[WorkerGuard] Failed to listen on socket for worker {}: {}", index, strerror(errno));
    close(listen_fd);
    return false;
  }

  std::vector<std::string> args = BuildWorkerArgs("--socket-name=" + sock_name, index);
  auto cmd = ToArgv(args);

  int options = subprocess_option_inherit_environment | subprocess_option_combined_stdout_stderr;
  auto proc = MakeSubprocess();
  int r = subprocess_create(cmd.data(), options, proc.get());
  if (r != 0) {
    LOG_ERROR("[WorkerGuard] Failed to start worker binary {}: error={}", index, r);
    close(listen_fd);
    return false;
  }

  // Parent process: wait for child to connect with timeout
  struct pollfd pfd;
  pfd.fd = listen_fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int pret = poll(&pfd, 1, 3000);
  if (pret <= 0 || !(pfd.revents & POLLIN)) {
    LOG_ERROR("[WorkerGuard] Worker {} connect timeout or failed", index);
    close(listen_fd);
    return false;
  }

  int client_fd = accept(listen_fd, nullptr, nullptr);
  close(listen_fd);

  if (client_fd < 0) {
    LOG_ERROR("[WorkerGuard] Failed to accept connection for worker {}: {}", index, strerror(errno));
    return false;
  }

  pid_t pid = proc->child;
  workers_[index].socket_fd = client_fd;
  FinalizeSpawnedWorker(index, std::move(proc), pid);
  return true;
}
#endif

bool WorkerGuard::SpawnWorker(size_t index) {
  if (index >= workers_.size()) {
    return false;
  }
#ifdef _WIN32
  return SpawnWorkerWindows(index);
#else
  return SpawnWorkerPosix(index);
#endif
}

void WorkerGuard::KillWorker(size_t index, TaskStatus status, const std::string &reason) {
  (void)status;
  if (index >= workers_.size()) {
    return;
  }
  WorkerInstance &inst = workers_[index];
  LOG_WARN("[WorkerGuard] KillWorker index={}: reason={}", index, reason);
  if (inst.is_busy()) {
    shm_pool_.ReleaseSlot(inst.current_slot_id);
  }
  inst.Reset();
}

void WorkerGuard::FinalizeBusyWorkerOnFailure(size_t worker_idx, uint32_t slot_id, TaskStatus status,
                                              const std::string &reason) {
  WorkerInstance &worker = workers_[worker_idx];
  const bool watchdog_handled =
      (worker.state == WorkerState::kKilledByWatchdog || worker.state == WorkerState::kCrashed);
  if (!watchdog_handled) {
    shm_pool_.ReleaseSlot(slot_id);
    KillWorker(worker_idx, status, reason);
  }
  worker.state = WorkerState::kIdle;
  worker.idle_start_time = std::chrono::steady_clock::now();
}

void WorkerGuard::WatchdogLoop() {
  while (is_running_.load(std::memory_order_acquire)) {
    {
      std::unique_lock<std::mutex> cv_lock(watchdog_mutex_);
      watchdog_cv_.wait_for(cv_lock, std::chrono::milliseconds(50),
                            [this]() { return !is_running_.load(std::memory_order_acquire); });
    }
    if (!is_running_.load(std::memory_order_acquire)) {
      break;
    }

    std::unique_lock<std::mutex> lock(pool_mutex_);
    auto now = std::chrono::steady_clock::now();

    for (size_t i = 0; i < workers_.size(); ++i) {
      WorkerInstance &inst = workers_[i];
      if (!inst.process || inst.pid <= 0) {
        continue;
      }

      // 1. Check if child crashed or exited unexpectedly
      if (!subprocess_alive(inst.process.get())) {
        int exit_code = 0;
        if (inst.process) {
          subprocess_join(inst.process.get(), &exit_code);
        }
        LOG_WARN("[WorkerGuard] Worker {} (pid {}) died unexpectedly: exit_code={}", i, inst.pid, exit_code);
        const bool was_busy = inst.is_busy();
        if (was_busy) {
          shm_pool_.ReleaseSlot(inst.current_slot_id);
        }
        // Defer returning to kIdle until the owning ExecuteTask finishes cleanup so no other task reuses slot
        // i/slot_id.
        inst.Reset(was_busy ? WorkerState::kCrashed : WorkerState::kIdle);
        if (!was_busy) {
          worker_available_cv_.notify_all();
        }
        continue;
      }

      // 2. Check for task timeout or stall heartbeat
      if (inst.state == WorkerState::kBusy) {
        uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - inst.task_start_time).count();

        uint64_t last_hb_ms = shm_pool_.GetSlotLastHeartbeat(inst.current_slot_id);
        bool is_stalled = (last_hb_ms > 0 && (now_ms > last_hb_ms) && (now_ms - last_hb_ms > kDefaultStallTimeoutMs));
        bool is_overtime = (elapsed > inst.current_timeout_ms);

        if (is_stalled || is_overtime) {
          LOG_WARN(
              "[WorkerGuard] Watchdog killing worker {}: is_stalled={}, is_overtime={}, elapsed={}ms, timeout={}ms", i,
              is_stalled, is_overtime, elapsed, inst.current_timeout_ms);
          shm_pool_.ReleaseSlot(inst.current_slot_id);
          inst.Reset(WorkerState::kKilledByWatchdog);
          continue;
        }
      }

      // 3. Check idle eviction or memory limit when idle
      if (inst.state == WorkerState::kIdle && inst.pid > 0) {
        auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - inst.idle_start_time).count();
        if (idle_ms >= idle_evict_ms_) {
          LOG_INFO("[WorkerGuard] Worker {} idle for {:.1f}s. Evicted to reclaim memory.", i, idle_ms / 1000.0);
          inst.Reset();
          continue;
        }

        size_t rss = GetProcessRssBytes(inst.pid);
        if (rss > kDefaultMaxMemoryBytes) {
          KillWorker(i, TaskStatus::kSuccess, "Memory limit exceeded; recycling");
          worker_available_cv_.notify_all();
        }
      }
    }

    if (!workers_.empty() && shm_pool_.has_dirty_pages()) {
      bool any_active = false;
      for (const auto &inst : workers_) {
        if (inst.pid > 0 || inst.is_busy()) {
          any_active = true;
          break;
        }
      }
      if (!any_active && shm_pool_.ReclaimIdlePages()) {
        LOG_INFO("[WorkerGuard] All workers evicted; reclaimed idle shared memory pages.");
      }
    }
  }
}

bool WorkerGuard::ExecuteTask(const ParseTask &task, ParseTaskResult *out_result) {
  if (!out_result) {
    return false;
  }
  out_result->doc_id = task.doc_id;
  out_result->status = TaskStatus::kFailed;
  out_result->slot_id = 0;
  out_result->extracted_bytes = 0;

  uint32_t slot_id = 0;
  uint8_t *slot_data = nullptr;

  size_t assigned_worker_idx = 0;
#ifdef _WIN32
  HANDLE worker_pipe = nullptr;
#else
  int worker_fd = -1;
#endif

  auto start_time = std::chrono::steady_clock::now();

  {
    std::unique_lock<std::mutex> lock(pool_mutex_);

    // Wait until both an idle worker slot AND an available SHM slot exist
    worker_available_cv_.wait(lock, [this, &assigned_worker_idx, &slot_id, &slot_data]() {
      if (!is_running_.load(std::memory_order_acquire)) {
        return true;
      }
      for (size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i].state == WorkerState::kIdle) {
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

    WorkerInstance &worker = workers_[assigned_worker_idx];
    if (worker.pid <= 0) {
      if (!SpawnWorker(assigned_worker_idx)) {
        shm_pool_.ReleaseSlot(slot_id);
        out_result->status = TaskStatus::kFailed;
        out_result->error_message = "Failed to spawn worker on demand";
        worker_available_cv_.notify_all();
        return true;
      }
    }
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
    FinalizeBusyWorkerOnFailure(assigned_worker_idx, slot_id, TaskStatus::kFailed, "Pipe write failure");
    out_result->status = TaskStatus::kFailed;
    out_result->error_message = "Failed to send request to worker pipe";
    worker_available_cv_.notify_all();
    return true;
  }
#else
  try {
    capnp::writeMessageToFd(worker_fd, req_builder);
  } catch (...) {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    FinalizeBusyWorkerOnFailure(assigned_worker_idx, slot_id, TaskStatus::kFailed, "Socket write failure");
    out_result->status = TaskStatus::kFailed;
    out_result->error_message = "Failed to send request to worker socket";
    worker_available_cv_.notify_all();
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
      auto wait_elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_start).count();
      if (wait_elapsed >= wait_timeout) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (bytes_avail > 0) {
      kj::HandleInputStream in_stream(worker_pipe);
      capnp::InputStreamMessageReader resp_reader(in_stream);
      auto resp = resp_reader.getRoot<soeasy::ipc::ParseResult>();

      out_result->status =
          (resp.getStatusCode() == soeasy::ipc::ParseStatusCode::OK) ? TaskStatus::kSuccess : TaskStatus::kFailed;
      out_result->slot_id = slot_id;
      out_result->extracted_bytes = resp.getExtractedBytes();
      out_result->is_scanned_pdf = resp.getIsScannedPdf();
      out_result->error_message = resp.getMessage();
      if (resp.hasExtractedText()) {
        out_result->extracted_text = resp.getExtractedText().cStr();
      }
      if (resp.hasChunks()) {
        auto chunks_reader = resp.getChunks();
        out_result->stream_chunks.reserve(chunks_reader.size());
        size_t total_chunk_bytes = 0;
        for (auto chunk : chunks_reader) {
          out_result->stream_chunks.emplace_back(reinterpret_cast<const char *>(chunk.begin()), chunk.size());
          total_chunk_bytes += chunk.size();
        }
        if (out_result->extracted_text.empty() && total_chunk_bytes > 0) {
          out_result->extracted_text.reserve(total_chunk_bytes);
          for (const auto &c : out_result->stream_chunks) {
            out_result->extracted_text.append(c);
          }
        }
      }
      std::string msg = resp.getMessage();
      if (msg.rfind("SPILL:", 0) == 0) {
        out_result->spill_file_path = msg.substr(6);
      }
      success_response = true;
    }
  } catch (...) {
    success_response = false;
  }
#else
  try {
    capnp::StreamFdMessageReader resp_reader(worker_fd);
    auto resp = resp_reader.getRoot<soeasy::ipc::ParseResult>();

    out_result->status =
        (resp.getStatusCode() == soeasy::ipc::ParseStatusCode::OK) ? TaskStatus::kSuccess : TaskStatus::kFailed;
    out_result->slot_id = slot_id;
    out_result->extracted_bytes = resp.getExtractedBytes();
    out_result->is_scanned_pdf = resp.getIsScannedPdf();
    out_result->error_message = resp.getMessage();
    if (resp.hasExtractedText()) {
      out_result->extracted_text = resp.getExtractedText().cStr();
    }
    if (resp.hasChunks()) {
      auto chunks_reader = resp.getChunks();
      out_result->stream_chunks.reserve(chunks_reader.size());
      size_t total_chunk_bytes = 0;
      for (auto chunk : chunks_reader) {
        out_result->stream_chunks.emplace_back(reinterpret_cast<const char *>(chunk.begin()), chunk.size());
        total_chunk_bytes += chunk.size();
      }
      if (out_result->extracted_text.empty() && total_chunk_bytes > 0) {
        out_result->extracted_text.reserve(total_chunk_bytes);
        for (const auto &c : out_result->stream_chunks) {
          out_result->extracted_text.append(c);
        }
      }
    }
    std::string msg = resp.getMessage();
    if (msg.rfind("SPILL:", 0) == 0) {
      out_result->spill_file_path = msg.substr(6);
    }
    success_response = true;
  } catch (const kj::Exception &e) {
    LOG_WARN("[WorkerGuard] kj::Exception reading from worker {}: {}", assigned_worker_idx, e.getDescription().cStr());
    success_response = false;
  } catch (const std::exception &e) {
    LOG_WARN("[WorkerGuard] std::exception reading from worker {}: {}", assigned_worker_idx, e.what());
    success_response = false;
  } catch (...) {
    LOG_WARN("[WorkerGuard] Unknown exception reading from worker {}", assigned_worker_idx);
    success_response = false;
  }
#endif

  auto finish_time = std::chrono::steady_clock::now();
  out_result->elapsed_ms = std::chrono::duration<double, std::milli>(finish_time - start_time).count();
  uint32_t effective_timeout = task.timeout_ms > 0 ? task.timeout_ms : timeout_ms_;
  bool is_timeout = (!success_response && out_result->elapsed_ms >= effective_timeout);

  {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    WorkerInstance &worker = workers_[assigned_worker_idx];

    if (worker.state == WorkerState::kKilledByWatchdog || (worker.state != WorkerState::kCrashed && is_timeout)) {
      out_result->status = TaskStatus::kTimeout;
      out_result->error_message = "Task timed out or stalled (killed by watchdog)";
      out_result->slot_id = 0;
      FinalizeBusyWorkerOnFailure(assigned_worker_idx, slot_id, TaskStatus::kTimeout, out_result->error_message);
    } else if (success_response) {
      worker.state = WorkerState::kIdle;
      worker.idle_start_time = std::chrono::steady_clock::now();
      worker.total_tasks_processed++;
      if (out_result->status != TaskStatus::kSuccess) {
        // Task returned FAILED from worker: release the slot immediately
        shm_pool_.ReleaseSlot(slot_id);
        out_result->slot_id = 0;
      }
    } else {
      out_result->status = TaskStatus::kCrashed;
      std::string diag = GetRecentWorkerStdio(assigned_worker_idx);
      if (!diag.empty()) {
        LOG_WARN("[WorkerGuard] Worker {} crash diagnosis (last stdio):\n  {}", assigned_worker_idx, diag);
        out_result->error_message = "Worker crashed (SIGSEGV/abort) during file parsing: " + diag;
      } else {
        out_result->error_message = "Worker crashed (SIGSEGV/abort) during file parsing";
      }
      out_result->slot_id = 0;
      FinalizeBusyWorkerOnFailure(assigned_worker_idx, slot_id, out_result->status, out_result->error_message);
    }
    worker_available_cv_.notify_all();
  }

  return true;
}

void WorkerGuard::Shutdown() {
  if (!is_running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  watchdog_cv_.notify_all();
  worker_available_cv_.notify_all();
  if (watchdog_thread_.joinable()) {
    watchdog_thread_.join();
  }

  std::unique_lock<std::mutex> lock(pool_mutex_);
  for (auto &inst : workers_) {
    inst.Reset();
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

void WorkerGuard::SetLoggingOptions(const WorkerLoggingOptions &options) {
  std::lock_guard<std::mutex> lock(pool_mutex_);
  const std::string &next_log_dir = options.worker_log_dir.empty() ? worker_log_dir_ : options.worker_log_dir;
  bool changed =
      (log_level_ != options.level || log_console_ != options.enable_console || log_sync_ != options.enable_sync ||
       doctotext_logging_ != options.doctotext_logging || doctotext_log_path_ != options.doctotext_log_path ||
       embedded_image_ocr_ != options.embedded_image_ocr || worker_log_dir_ != next_log_dir);
  log_level_ = options.level;
  log_console_ = options.enable_console;
  log_sync_ = options.enable_sync;
  doctotext_logging_ = options.doctotext_logging;
  doctotext_log_path_ = options.doctotext_log_path;
  embedded_image_ocr_ = options.embedded_image_ocr;
  worker_log_dir_ = next_log_dir;

  if (changed) {
    // Respawn idle workers with new arguments
    for (size_t i = 0; i < workers_.size(); ++i) {
      if (!workers_[i].is_busy() && workers_[i].pid > 0) {
        KillWorker(i, TaskStatus::kSuccess, "Options changed");
      }
    }
  }
}

void WorkerGuard::SetLoggingOptions(const std::string &level, bool enable_console, bool enable_sync,
                                    bool doctotext_logging, const std::string &doctotext_log_path,
                                    bool embedded_image_ocr, const std::string &worker_log_dir) {
  WorkerLoggingOptions opts;
  opts.level = level;
  opts.enable_console = enable_console;
  opts.enable_sync = enable_sync;
  opts.doctotext_logging = doctotext_logging;
  opts.doctotext_log_path = doctotext_log_path;
  opts.embedded_image_ocr = embedded_image_ocr;
  opts.worker_log_dir = worker_log_dir;
  SetLoggingOptions(opts);
}

}  // namespace service
}  // namespace soeasy
