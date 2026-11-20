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

#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "common/logger.h"
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

struct TaskRequest {
  ParseTask task;
  uint32_t timeout_ms = 15000;
  std::promise<ParseTaskResult> done_promise;
};

}  // namespace

class WorkerRpcChannel {
 public:
#ifdef _WIN32
  explicit WorkerRpcChannel(HANDLE pipe_handle) : pipe_handle_(pipe_handle) {
    thread_ = std::thread([this]() { Loop(); });
  }
#else
  explicit WorkerRpcChannel(int socket_fd) : socket_fd_(socket_fd) {
    thread_ = std::thread([this]() { Loop(); });
  }
#endif

  ~WorkerRpcChannel() {
    Stop();
  }

  WorkerRpcChannel(const WorkerRpcChannel &) = delete;
  WorkerRpcChannel &operator=(const WorkerRpcChannel &) = delete;

  ParseTaskResult Call(const ParseTask &task, uint32_t timeout_ms) {
    auto req = std::make_shared<TaskRequest>();
    req->task = task;
    req->timeout_ms = timeout_ms;
    auto fut = req->done_promise.get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) {
        ParseTaskResult res;
        res.doc_id = task.doc_id;
        res.status = TaskStatus::kCrashed;
        res.error_message = "Worker RPC channel stopped";
        return res;
      }
      queue_.push_back(req);
    }
    cv_.notify_one();
    return fut.get();
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
      thread_.join();
    }
  }

 private:
  void Loop() {
    kj::AsyncIoContext ioContext = kj::setupAsyncIo();
#ifdef _WIN32
    auto clientStream = ioContext.lowLevelProvider->wrapSocketFd(reinterpret_cast<uintptr_t>(pipe_handle_));
#else
    auto clientStream = ioContext.lowLevelProvider->wrapSocketFd(socket_fd_);
#endif
    capnp::TwoPartyClient client(*clientStream);
    auto agent = client.bootstrap().castAs<soeasy::ipc::WorkerAgent>();

    while (true) {
      std::shared_ptr<TaskRequest> req;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return stopped_ || !queue_.empty(); });
        if (stopped_ && queue_.empty()) {
          break;
        }
        req = queue_.front();
        queue_.pop_front();
      }

      ParseTaskResult result;
      result.doc_id = req->task.doc_id;

      try {
        auto rpcReq = agent.parseFileRequest();
        rpcReq.setDocId(req->task.doc_id);
        rpcReq.setFilePath(req->task.file_path);
        rpcReq.setExt(req->task.ext);
        rpcReq.setLevel(static_cast<soeasy::ipc::ParseLevel>(req->task.parse_level));

        auto sendPromise = rpcReq.send();
        auto timedPromise =
            ioContext.provider->getTimer().timeoutAfter(req->timeout_ms * kj::MILLISECONDS, kj::mv(sendPromise));
        auto resp = timedPromise.wait(ioContext.waitScope);

        auto parseRes = resp.getResult();
        result.status = (parseRes.getStatusCode() == soeasy::ipc::ParseStatusCode::OK) ? TaskStatus::kSuccess
                                                                                       : TaskStatus::kFailed;
        result.extracted_bytes = parseRes.getExtractedBytes();
        result.is_scanned_pdf = parseRes.getIsScannedPdf();
        result.error_message = parseRes.getMessage().cStr();
        if (parseRes.hasExtractedText()) {
          result.extracted_text = parseRes.getExtractedText().cStr();
        }
        // Split extracted_text into 256KB chunks if text exceeds 256KB for callers that inspect chunks
        constexpr size_t kChunkSize = 256 * 1024;
        if (result.extracted_text.size() > kChunkSize) {
          result.stream_chunks.reserve((result.extracted_text.size() + kChunkSize - 1) / kChunkSize);
          for (size_t off = 0; off < result.extracted_text.size(); off += kChunkSize) {
            size_t len = std::min(kChunkSize, result.extracted_text.size() - off);
            result.stream_chunks.emplace_back(result.extracted_text.substr(off, len));
          }
        }
        req->done_promise.set_value(std::move(result));
      } catch (const kj::Exception &e) {
        std::string desc = e.getDescription().cStr();
        if (desc.find("timed out") != std::string::npos || desc.find("timeout") != std::string::npos) {
          result.status = TaskStatus::kTimeout;
          result.error_message = "Task timed out: " + desc;
        } else {
          result.status = TaskStatus::kCrashed;
          result.error_message = "Worker peer disconnected: " + desc;
        }
        req->done_promise.set_value(std::move(result));
        if (result.status == TaskStatus::kCrashed) {
          break;
        }
      } catch (const std::exception &e) {
        result.status = TaskStatus::kCrashed;
        result.error_message = std::string("Worker exception: ") + e.what();
        req->done_promise.set_value(std::move(result));
        break;
      } catch (...) {
        result.status = TaskStatus::kCrashed;
        result.error_message = "Worker unknown exception";
        req->done_promise.set_value(std::move(result));
        break;
      }
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
      while (!queue_.empty()) {
        auto pending = queue_.front();
        queue_.pop_front();
        ParseTaskResult res;
        res.doc_id = pending->task.doc_id;
        res.status = TaskStatus::kCrashed;
        res.error_message = "Worker RPC channel terminated";
        pending->done_promise.set_value(std::move(res));
      }
    }
  }

#ifdef _WIN32
  HANDLE pipe_handle_ = nullptr;
#else
  int socket_fd_ = -1;
#endif
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<TaskRequest>> queue_;
  bool stopped_ = false;
};

WorkerInstance::WorkerInstance() = default;
WorkerInstance::~WorkerInstance() { Reset(); }

WorkerInstance::WorkerInstance(WorkerInstance &&other) noexcept
    : process(std::move(other.process)),
      stdio_drain_thread(std::move(other.stdio_drain_thread)),
      pid(other.pid),
#ifdef _WIN32
      pipe_handle(other.pipe_handle),
#else
      socket_fd(other.socket_fd),
#endif
      rpc_channel(std::move(other.rpc_channel)),
      state(other.state),
      current_doc_id(other.current_doc_id),
      current_timeout_ms(other.current_timeout_ms),
      task_start_time(other.task_start_time),
      idle_start_time(other.idle_start_time),
      total_tasks_processed(other.total_tasks_processed) {
  other.pid = 0;
#ifdef _WIN32
  other.pipe_handle = nullptr;
#else
  other.socket_fd = -1;
#endif
  other.state = WorkerState::kIdle;
  other.current_doc_id = 0;
}

WorkerInstance &WorkerInstance::operator=(WorkerInstance &&other) noexcept {
  if (this != &other) {
    Reset();
    process = std::move(other.process);
    stdio_drain_thread = std::move(other.stdio_drain_thread);
    pid = other.pid;
#ifdef _WIN32
    pipe_handle = other.pipe_handle;
    other.pipe_handle = nullptr;
#else
    socket_fd = other.socket_fd;
    other.socket_fd = -1;
#endif
    rpc_channel = std::move(other.rpc_channel);
    state = other.state;
    current_doc_id = other.current_doc_id;
    current_timeout_ms = other.current_timeout_ms;
    task_start_time = other.task_start_time;
    idle_start_time = other.idle_start_time;
    total_tasks_processed = other.total_tasks_processed;
    other.pid = 0;
    other.state = WorkerState::kIdle;
    other.current_doc_id = 0;
  }
  return *this;
}

void WorkerInstance::CloseTransport() noexcept {
  if (rpc_channel) {
    rpc_channel->Stop();
    rpc_channel.reset();
  }
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
                       size_t /*deprecated_slot_count*/, size_t /*deprecated_slot_capacity*/) {
  Shutdown();

#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  worker_bin_path_ = ResolveWorkerBinary(worker_bin_path);
  if (worker_bin_path_.empty()) {
    LOG_ERROR(
        "[WorkerGuard] Failed to locate soeasy-worker binary (hint: '{}'). Multithreaded in-process fork is "
        "prohibited.",
        worker_bin_path);
    return false;
  }
  timeout_ms_ = timeout_ms;

#ifdef _WIN32
  job_handle_ = CreateJobObjectA(NULL, NULL);
  if (job_handle_) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    ZeroMemory(&jeli, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_handle_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
  }
#endif

  is_running_.store(true, std::memory_order_release);
  target_worker_count_ = worker_count;
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
  workers_[index].rpc_channel = std::make_shared<WorkerRpcChannel>(hPipe);
  FinalizeSpawnedWorker(index, std::move(proc), pid);
  return true;
}
#else
bool WorkerGuard::SpawnWorkerPosix(size_t index) {
#ifdef SOCK_CLOEXEC
  int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd < 0) {
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  }
#else
  int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
#endif
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

#if defined(__linux__) && defined(SOCK_CLOEXEC)
  int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
#else
  int client_fd = accept(listen_fd, nullptr, nullptr);
  if (client_fd >= 0) {
    fcntl(client_fd, F_SETFD, FD_CLOEXEC);
  }
#endif
  close(listen_fd);

  if (client_fd < 0) {
    LOG_ERROR("[WorkerGuard] Failed to accept connection for worker {}: {}", index, strerror(errno));
    return false;
  }

  pid_t pid = proc->child;
  workers_[index].socket_fd = client_fd;
  workers_[index].rpc_channel = std::make_shared<WorkerRpcChannel>(client_fd);
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
  inst.Reset();
}

void WorkerGuard::FinalizeBusyWorkerOnFailure(size_t worker_idx, TaskStatus status,
                                              const std::string &reason) {
  WorkerInstance &worker = workers_[worker_idx];
  const bool watchdog_handled =
      (worker.state == WorkerState::kKilledByWatchdog || worker.state == WorkerState::kCrashed);
  if (!watchdog_handled) {
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
        inst.Reset(was_busy ? WorkerState::kCrashed : WorkerState::kIdle);
        if (!was_busy) {
          worker_available_cv_.notify_all();
        }
        continue;
      }

      // 2. Check for task timeout
      if (inst.state == WorkerState::kBusy) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - inst.task_start_time).count();
        bool is_overtime = (elapsed > inst.current_timeout_ms + 1000);

        if (is_overtime) {
          LOG_WARN(
              "[WorkerGuard] Watchdog killing worker {}: elapsed={}ms, timeout={}ms", i,
              elapsed, inst.current_timeout_ms);
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
  out_result->extracted_text.clear();
  out_result->stream_chunks.clear();

  size_t assigned_worker_idx = 0;
  auto start_time = std::chrono::steady_clock::now();

  {
    std::unique_lock<std::mutex> lock(pool_mutex_);

    worker_available_cv_.wait(lock, [this, &assigned_worker_idx]() {
      if (!is_running_.load(std::memory_order_acquire)) {
        return true;
      }
      size_t limit = (target_worker_count_ > 0) ? std::min(workers_.size(), target_worker_count_) : workers_.size();
      for (size_t i = 0; i < limit; ++i) {
        if (workers_[i].state == WorkerState::kIdle) {
          assigned_worker_idx = i;
          return true;
        }
      }
      return false;
    });

    if (!is_running_.load(std::memory_order_acquire)) {
      return false;
    }

    WorkerInstance &worker = workers_[assigned_worker_idx];
    if (worker.pid <= 0 || !worker.rpc_channel) {
      if (!SpawnWorker(assigned_worker_idx)) {
        out_result->status = TaskStatus::kFailed;
        out_result->error_message = "Failed to spawn worker on demand";
        worker_available_cv_.notify_all();
        return true;
      }
    }
    worker.state = WorkerState::kBusy;
    worker.current_doc_id = task.doc_id;
    worker.current_timeout_ms = task.timeout_ms > 0 ? task.timeout_ms : timeout_ms_;
    worker.task_start_time = start_time;
  }

  // Execute RPC call to worker channel
  ParseTaskResult rpc_res;
  bool rpc_success = false;
  {
    std::shared_ptr<WorkerRpcChannel> chan;
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      chan = workers_[assigned_worker_idx].rpc_channel;
    }
    if (chan) {
      rpc_res = chan->Call(task, task.timeout_ms > 0 ? task.timeout_ms : timeout_ms_);
      rpc_success = true;
    }
  }

  auto finish_time = std::chrono::steady_clock::now();
  double elapsed_ms = std::chrono::duration<double, std::milli>(finish_time - start_time).count();

  {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    WorkerInstance &worker = workers_[assigned_worker_idx];

    if (!rpc_success || rpc_res.status == TaskStatus::kCrashed) {
      out_result->status = TaskStatus::kCrashed;
      std::string diag = GetRecentWorkerStdio(assigned_worker_idx);
      if (!diag.empty()) {
        LOG_WARN("[WorkerGuard] Worker {} crash diagnosis (last stdio):\n  {}", assigned_worker_idx, diag);
        out_result->error_message = "Worker crashed (SIGSEGV/abort) during file parsing: " + diag;
      } else {
        out_result->error_message = rpc_res.error_message.empty()
                                       ? "Worker crashed (SIGSEGV/abort) during file parsing"
                                       : rpc_res.error_message;
      }
      FinalizeBusyWorkerOnFailure(assigned_worker_idx, TaskStatus::kCrashed, out_result->error_message);
    } else if (worker.state == WorkerState::kKilledByWatchdog || rpc_res.status == TaskStatus::kTimeout) {
      out_result->status = TaskStatus::kTimeout;
      out_result->error_message = "Task timed out or stalled (killed by watchdog)";
      FinalizeBusyWorkerOnFailure(assigned_worker_idx, TaskStatus::kTimeout, out_result->error_message);
    } else {
      *out_result = std::move(rpc_res);
      worker.state = WorkerState::kIdle;
      worker.idle_start_time = std::chrono::steady_clock::now();
      worker.total_tasks_processed++;
    }
    out_result->elapsed_ms = elapsed_ms;
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

void WorkerGuard::SetWorkerCount(size_t new_count) {
  if (new_count == 0) {
    new_count = 1;
  }
  std::unique_lock<std::mutex> lock(pool_mutex_);
  if (!is_running_.load(std::memory_order_acquire)) {
    return;
  }
  size_t current_count = target_worker_count_ > 0 ? target_worker_count_ : workers_.size();
  if (new_count == current_count && workers_.size() == new_count) {
    return;
  }

  target_worker_count_ = new_count;

  if (new_count > workers_.size()) {
    size_t old_size = workers_.size();
    workers_.resize(new_count);
    {
      std::lock_guard<std::mutex> stdio_lock(stdio_mutex_);
      recent_stdio_.resize(new_count);
    }
    auto now = std::chrono::steady_clock::now();
    for (size_t i = old_size; i < new_count; ++i) {
      workers_[i].pid = 0;
      workers_[i].state = WorkerState::kIdle;
      workers_[i].idle_start_time = now;
    }
    LOG_INFO("[WorkerGuard] Dynamically scaled UP worker pool from {} to {}", current_count, new_count);
    worker_available_cv_.notify_all();
  } else {
    LOG_INFO("[WorkerGuard] Dynamically scaling DOWN worker pool from {} to {}", current_count, new_count);
    // 1. Immediately kill any idle workers beyond new_count
    for (size_t i = new_count; i < workers_.size(); ++i) {
      if (workers_[i].state == WorkerState::kIdle && workers_[i].pid > 0) {
        KillWorker(i, TaskStatus::kSuccess, "Scaled down by user config");
      }
    }
    // 2. Wait for any busy workers in [new_count, workers_.size()) to finish their ongoing task.
    while (true) {
      bool any_busy = false;
      for (size_t i = new_count; i < workers_.size(); ++i) {
        if (workers_[i].is_busy()) {
          any_busy = true;
          break;
        }
      }
      if (!any_busy) {
        break;
      }
      worker_available_cv_.wait_for(lock, std::chrono::milliseconds(10));
    }
    // 3. Clean up processes of workers that finished
    for (size_t i = new_count; i < workers_.size(); ++i) {
      if (workers_[i].pid > 0) {
        KillWorker(i, TaskStatus::kSuccess, "Scaled down by user config");
      }
    }
    workers_.resize(new_count);
    {
      std::lock_guard<std::mutex> stdio_lock(stdio_mutex_);
      recent_stdio_.resize(new_count);
    }
    LOG_INFO("[WorkerGuard] Worker pool successfully scaled DOWN to {}", new_count);
    worker_available_cv_.notify_all();
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
