#include "service/worker_guard.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "service/worker_rpc_channel.h"

namespace soeasy {
namespace service {

namespace {

int64_t SteadyEpochMs(std::chrono::steady_clock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

}  // namespace

DetachedWorkerResources &DetachedWorkerResources::operator=(DetachedWorkerResources &&other) noexcept {
  if (this != &other) {
    Reset();
    process = std::move(other.process);
    stdio_drain_thread = std::move(other.stdio_drain_thread);
    rpc_channel = std::move(other.rpc_channel);
  }
  return *this;
}

void DetachedWorkerResources::Reset() noexcept {
  if (rpc_channel) {
    rpc_channel->Stop();
    rpc_channel.reset();
  }
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
}

WorkerInstance::~WorkerInstance() {
  Detach(WorkerState::kIdle).Reset();
}

DetachedWorkerResources WorkerInstance::Detach(WorkerState next_state) noexcept {
  DetachedWorkerResources res;
  res.rpc_channel = std::move(rpc_channel);
  res.process = std::move(process);
  res.stdio_drain_thread = std::move(stdio_drain_thread);
  pid = 0;
  tasks_since_mem_check = 0;
  state = next_state;
  return res;
}

WorkerGuard::WorkerGuard() = default;

WorkerGuard::~WorkerGuard() {
  Shutdown();
}

size_t WorkerGuard::worker_count() const {
  std::lock_guard<std::mutex> lock(pool_mutex_);
  return target_worker_count_ > 0 ? target_worker_count_ : workers_.size();
}

size_t WorkerGuard::active_worker_count() const {
  std::lock_guard<std::mutex> lock(pool_mutex_);
  size_t count = 0;
  for (const auto &w : workers_) {
    if (w && w->pid > 0) count++;
  }
  return count;
}

bool WorkerGuard::Init(const std::string &worker_bin_path, size_t worker_count, uint32_t timeout_ms) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  ShutdownLocked();

#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif

  if (worker_count == 0) {
    worker_count = 1;
  }

  std::string resolved_bin = ResolveWorkerBinary(worker_bin_path);
  if (resolved_bin.empty()) {
    LOG_ERROR(
        "[WorkerGuard] Failed to locate soeasy-worker binary (hint: '{}'). Multithreaded in-process fork is "
        "prohibited.",
        worker_bin_path);
    return false;
  }
  timeout_ms_.store(timeout_ms > 0 ? timeout_ms : kDefaultTimeoutMs, std::memory_order_relaxed);
  memory_recycle_count_.store(0, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    worker_bin_path_ = std::move(resolved_bin);
#ifdef _WIN32
    job_handle_ = CreateJobObjectA(NULL, NULL);
    if (job_handle_) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
      ZeroMemory(&jeli, sizeof(jeli));
      jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job_handle_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
#endif
    pool_generation_++;
    target_worker_count_ = worker_count;
    workers_.clear();
    workers_.reserve(worker_count);
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < worker_count; ++i) {
      auto inst = std::make_unique<WorkerInstance>();
      inst->idle_start_time = now;
      workers_.push_back(std::move(inst));
    }
    unpacker_worker_ = std::make_unique<WorkerInstance>();
    unpacker_worker_->idle_start_time = now;
    unpacker_last_activity_ms_.store(SteadyEpochMs(now), std::memory_order_release);
    unpacker_in_callback_.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> stdio_lock(stdio_mutex_);
      recent_stdio_.assign(worker_count, {});
      unpacker_recent_stdio_.clear();
    }
    is_running_.store(true, std::memory_order_release);
  }

  try {
    watchdog_thread_ = std::thread(&WorkerGuard::WatchdogLoop, this);
  } catch (const std::exception &e) {
    LOG_ERROR("[WorkerGuard] Failed to start watchdog thread: {}", e.what());
    ShutdownLocked();
    return false;
  }
  return true;
}

std::vector<std::string> WorkerGuard::BuildWorkerArgs(const std::string &ipc_param, size_t index) const {
  const std::string log_dir = worker_log_dir_.empty() ? "logs" : worker_log_dir_;
  const std::string worker_log_file = DeriveWorkerLogPath(log_dir, index);
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

void WorkerGuard::ClearWorkerStdio(size_t index) {
  std::lock_guard<std::mutex> lock(stdio_mutex_);
  if (index == kUnpackerWorkerIndex) {
    unpacker_recent_stdio_.clear();
    return;
  }
  if (index < recent_stdio_.size()) {
    recent_stdio_[index].clear();
  }
}

void WorkerGuard::RecordWorkerStdio(size_t index, const std::string &line) {
  std::lock_guard<std::mutex> lock(stdio_mutex_);
  if (index == kUnpackerWorkerIndex) {
    if (unpacker_recent_stdio_.size() >= 30) {
      unpacker_recent_stdio_.pop_front();
    }
    unpacker_recent_stdio_.push_back(line);
    return;
  }
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
  const std::deque<std::string> *buf = nullptr;
  if (index == kUnpackerWorkerIndex) {
    buf = &unpacker_recent_stdio_;
  } else if (index < recent_stdio_.size()) {
    buf = &recent_stdio_[index];
  } else {
    return {};
  }
  std::string out;
  for (const auto &l : *buf) {
    if (!out.empty()) out += "\n  ";
    out += l;
  }
  return out;
}

std::string WorkerGuard::BuildCrashErrorMessage(size_t index, const std::string &rpc_error,
                                                const char *default_msg) const {
  const std::string label = (index == kUnpackerWorkerIndex) ? "Unpacker" : ("Worker " + std::to_string(index));
  const std::string diag = GetRecentWorkerStdio(index);
  if (!diag.empty()) {
    LOG_WARN("[WorkerGuard] {} crash diagnosis (last stdio):\n  {}", label, diag);
    return std::string(default_msg) + ": " + diag;
  }
  return rpc_error.empty() ? std::string(default_msg) : rpc_error;
}

void WorkerGuard::DrainWorkerStdio(WorkerGuard *guard, FILE *stream, size_t index, bool log_as_info) {
  if (!stream) return;
  const std::string tag = (index == kUnpackerWorkerIndex) ? "Unpacker" : ("Worker " + std::to_string(index));
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
      LOG_INFO("[{} stdio] {}", tag, buf);
    } else {
      LOG_DEBUG("[{} stdio] {}", tag, buf);
    }
  }
}

bool WorkerGuard::SpawnWorkerProcess(size_t index, DetachedWorkerResources *out_resources, WorkerPid *out_pid,
                                       const std::vector<std::string> &args_template, bool log_as_info) {
  ClearWorkerStdio(index);
  constexpr uint32_t kHandshakeTimeoutMs = 5000;
  auto channel = std::make_shared<WorkerRpcChannel>(kHandshakeTimeoutMs);
  const uint16_t port = channel->listening_port();
  if (port == 0) {
    LOG_ERROR("[WorkerGuard] Failed to bind Cap'n Proto RPC listener for worker {}", index);
    channel->Stop();
    return false;
  }

  std::vector<std::string> args = args_template;
  if (args.size() > 1) {
    args[1] = "--rpc-addr=127.0.0.1:" + std::to_string(port);
  }
  auto cmd = ToArgv(args);

#ifdef _WIN32
  int options =
      subprocess_option_inherit_environment | subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
#else
  int options = subprocess_option_inherit_environment | subprocess_option_combined_stdout_stderr;
#endif
  auto proc = MakeSubprocess();
  int r = subprocess_create(cmd.data(), options, proc.get());
  if (r != 0) {
    LOG_ERROR("[WorkerGuard] subprocess_create failed to start worker {}: error={}", index, r);
    channel->Stop();
    return false;
  }

#ifdef _WIN32
  WorkerPid pid = static_cast<WorkerPid>(GetProcessId(proc->hProcess));
#else
  WorkerPid pid = static_cast<WorkerPid>(proc->child);
#endif

  // Start draining stdout/stderr before waiting for RPC handshake to prevent pipe-buffer startup deadlocks.
  DetachedWorkerResources resources;
  resources.rpc_channel = std::move(channel);
  resources.process = std::move(proc);
  if (resources.process && resources.process->stdout_file) {
    resources.stdio_drain_thread =
        std::thread(&WorkerGuard::DrainWorkerStdio, this, resources.process->stdout_file, index, log_as_info);
  }

  if (pid <= 0) {
    LOG_ERROR("[WorkerGuard] Worker {} returned invalid PID {}", index, pid);
    return false;
  }

  if (!resources.rpc_channel->WaitConnected(kHandshakeTimeoutMs)) {
    LOG_ERROR("[WorkerGuard] Worker {} RPC handshake timeout", index);
    return false;
  }

  *out_resources = std::move(resources);
  *out_pid = pid;
  return true;
}

WorkerInstance *WorkerGuard::GetSlotLocked(size_t index) {
  if (index == kUnpackerWorkerIndex) {
    return unpacker_worker_.get();
  }
  return index < workers_.size() ? workers_[index].get() : nullptr;
}

void WorkerGuard::ReleaseRecycledSlots(const size_t *indices, size_t count, uint64_t expected_generation,
                                       bool clear_watchdog_pending) {
  if (!indices || count == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(pool_mutex_);
  if (!is_running_.load(std::memory_order_acquire) || pool_generation_ != expected_generation) {
    return;
  }
  const auto idle_now = std::chrono::steady_clock::now();
  bool released_any = false;
  for (size_t k = 0; k < count; ++k) {
    const size_t idx = indices[k];
    WorkerInstance *slot = GetSlotLocked(idx);
    if (!slot) {
      continue;
    }
    if (clear_watchdog_pending) {
      slot->watchdog_cleanup_pending = false;
    }
    if (slot->state == WorkerState::kRecycling && !slot->watchdog_cleanup_pending) {
      slot->state = WorkerState::kIdle;
      slot->idle_start_time = idle_now;
      released_any = true;
    }
  }
  if (released_any) {
    worker_available_cv_.notify_all();
  }
}

void WorkerGuard::MonitorSlotLocked(size_t slot_idx, WorkerInstance &inst, std::chrono::steady_clock::time_point now,
                                    int64_t now_epoch_ms, bool should_check_idle_mem,
                                    std::vector<DetachedWorkerResources> *to_cleanup,
                                    std::vector<size_t> *recycled_indices) {
  if (!inst.process || inst.pid <= 0) {
    return;
  }
  const bool is_unpacker = (slot_idx == kUnpackerWorkerIndex);
  const std::string label = is_unpacker ? "Unpacker" : ("Worker " + std::to_string(slot_idx));

  // 1. Check if child crashed or exited unexpectedly
  if (!subprocess_alive(inst.process.get())) {
    int exit_code = 0;
    subprocess_join(inst.process.get(), &exit_code);
    LOG_WARN("[WorkerGuard] {} (pid {}) died unexpectedly: exit_code={}", label, inst.pid, exit_code);
    const bool was_busy = inst.is_busy();
    if (was_busy) {
      inst.watchdog_cleanup_pending = true;
    }
    to_cleanup->push_back(inst.Detach(was_busy ? WorkerState::kCrashed : WorkerState::kRecycling));
    recycled_indices->push_back(slot_idx);
    return;
  }

  // 2. Check for task timeout (Unpacker uses inactivity watchdog and pauses while parent handles backpressure)
  if (inst.state == WorkerState::kBusy) {
    int64_t elapsed = 0;
    if (is_unpacker) {
      if (unpacker_in_callback_.load(std::memory_order_acquire)) {
        elapsed = 0;
      } else {
        elapsed = now_epoch_ms - unpacker_last_activity_ms_.load(std::memory_order_acquire);
      }
    } else {
      elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - inst.task_start_time).count();
    }
    if (elapsed > static_cast<int64_t>(inst.current_timeout_ms) + 1000) {
      LOG_WARN("[WorkerGuard] Watchdog killing {}: elapsed={}ms, timeout={}ms", label, elapsed,
               inst.current_timeout_ms);
      inst.watchdog_cleanup_pending = true;
      to_cleanup->push_back(inst.Detach(WorkerState::kKilledByWatchdog));
      recycled_indices->push_back(slot_idx);
      return;
    }
  }

  // 3. Check idle eviction or memory limit when idle
  if (inst.state == WorkerState::kIdle && inst.pid > 0) {
    auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - inst.idle_start_time).count();
    if (idle_ms >= static_cast<int64_t>(idle_evict_ms_.load(std::memory_order_relaxed))) {
      LOG_INFO("[WorkerGuard] {} idle for {:.1f}s. Evicted to reclaim memory.", label, idle_ms / 1000.0);
      to_cleanup->push_back(inst.Detach(WorkerState::kRecycling));
      recycled_indices->push_back(slot_idx);
      return;
    }

    const size_t max_mem = max_memory_bytes_.load(std::memory_order_relaxed);
    if (should_check_idle_mem && max_mem > 0) {
      const size_t rss = GetProcessRssBytes(inst.pid);
      if (rss > max_mem) {
        LOG_WARN("[WorkerGuard] {} (pid {}) exceeded memory limit while idle ({:.1f} MiB > {:.1f} MiB); recycling",
                 label, inst.pid, rss / (1024.0 * 1024.0), max_mem / (1024.0 * 1024.0));
        memory_recycle_count_.fetch_add(1, std::memory_order_relaxed);
        to_cleanup->push_back(inst.Detach(WorkerState::kRecycling));
        recycled_indices->push_back(slot_idx);
      }
    }
  }
}

void WorkerGuard::WatchdogLoop() {
  std::chrono::steady_clock::time_point last_idle_mem_check_time{};
  constexpr auto kIdleMemoryCheckInterval = std::chrono::milliseconds(500);

  while (is_running_.load(std::memory_order_acquire)) {
    {
      std::unique_lock<std::mutex> cv_lock(watchdog_mutex_);
      watchdog_cv_.wait_for(cv_lock, std::chrono::milliseconds(50),
                            [this]() { return !is_running_.load(std::memory_order_acquire); });
    }
    if (!is_running_.load(std::memory_order_acquire)) {
      break;
    }

    std::vector<DetachedWorkerResources> to_cleanup;
    std::vector<size_t> recycled_indices;
    uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lock(pool_mutex_);
      generation = pool_generation_;
      auto now = std::chrono::steady_clock::now();
      const int64_t now_epoch_ms = SteadyEpochMs(now);
      const bool should_check_idle_mem =
          (last_idle_mem_check_time == std::chrono::steady_clock::time_point{}) ||
          ((now - last_idle_mem_check_time) >= kIdleMemoryCheckInterval);
      if (should_check_idle_mem) {
        last_idle_mem_check_time = now;
      }

      for (size_t i = 0; i < workers_.size(); ++i) {
        MonitorSlotLocked(i, *workers_[i], now, now_epoch_ms, should_check_idle_mem, &to_cleanup, &recycled_indices);
      }
      if (unpacker_worker_) {
        MonitorSlotLocked(kUnpackerWorkerIndex, *unpacker_worker_, now, now_epoch_ms, should_check_idle_mem,
                          &to_cleanup, &recycled_indices);
      }
    }
    // Join detached worker processes and drain threads outside pool_mutex_ before unlocking slots.
    to_cleanup.clear();
    ReleaseRecycledSlots(recycled_indices.data(), recycled_indices.size(), generation, /*clear_watchdog_pending=*/true);
  }
}

bool WorkerGuard::ExecuteTask(const ParseTask &task, ParseTaskResult *out_result) {
  if (!out_result) {
    return false;
  }
  out_result->doc_id = task.doc_id;
  out_result->status = TaskStatus::kFailed;
  out_result->extracted_bytes = 0;
  out_result->is_scanned_pdf = false;
  out_result->error_message.clear();
  out_result->extracted_text.clear();
  out_result->stream_chunk_count = 0;
  out_result->elapsed_ms = 0.0;

  size_t assigned_worker_idx = 0;
  uint64_t generation = 0;
  uint32_t effective_timeout_ms = task.timeout_ms > 0 ? task.timeout_ms : timeout_ms_.load(std::memory_order_relaxed);
  if (effective_timeout_ms == 0) {
    effective_timeout_ms = kDefaultTimeoutMs;
  }
  bool need_spawn = false;
  std::shared_ptr<WorkerRpcChannel> chan;
  std::vector<std::string> spawn_args;
  bool spawn_log_as_info = false;
  std::chrono::steady_clock::time_point exec_start_time;

  {
    std::unique_lock<std::mutex> lock(pool_mutex_);

    worker_available_cv_.wait(lock, [this, &assigned_worker_idx]() {
      if (!is_running_.load(std::memory_order_acquire)) {
        return true;
      }
      size_t limit = (target_worker_count_ > 0) ? std::min(workers_.size(), target_worker_count_) : workers_.size();
      for (size_t i = 0; i < limit; ++i) {
        if (workers_[i]->state == WorkerState::kIdle) {
          assigned_worker_idx = i;
          return true;
        }
      }
      return false;
    });

    if (!is_running_.load(std::memory_order_acquire)) {
      return false;
    }

    generation = pool_generation_;
    exec_start_time = std::chrono::steady_clock::now();
    WorkerInstance &worker = *workers_[assigned_worker_idx];
    worker.state = WorkerState::kBusy;
    worker.current_timeout_ms = effective_timeout_ms;
    worker.task_start_time = exec_start_time;

    need_spawn = (worker.pid <= 0 || !worker.rpc_channel);
    if (need_spawn) {
      spawn_args = BuildWorkerArgs("", assigned_worker_idx);
      spawn_log_as_info = log_console_;
    } else {
      chan = worker.rpc_channel;
    }
  }

  // Spawn worker outside pool_mutex_ so handshake wait never stalls watchdog or other workers.
  if (need_spawn) {
    DetachedWorkerResources spawned;
    WorkerPid spawned_pid = 0;
    bool ok = SpawnWorkerProcess(assigned_worker_idx, &spawned, &spawned_pid, spawn_args, spawn_log_as_info);
    if (!ok) {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      const bool same_pool = is_running_.load(std::memory_order_acquire) && pool_generation_ == generation;
      if (same_pool && assigned_worker_idx < workers_.size()) {
        workers_[assigned_worker_idx]->state = WorkerState::kIdle;
      }
      out_result->status = TaskStatus::kFailed;
      out_result->error_message = "Failed to spawn worker on demand";
      worker_available_cv_.notify_all();
      return same_pool;
    }

    DetachedWorkerResources stale_resources;
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      if (!is_running_.load(std::memory_order_acquire) || pool_generation_ != generation ||
          assigned_worker_idx >= workers_.size()) {
        return false;
      }
      WorkerInstance &worker = *workers_[assigned_worker_idx];
      if (worker.HasResources()) {
        stale_resources = worker.Detach(WorkerState::kBusy);
      }
#ifdef _WIN32
      if (job_handle_ && spawned.process && spawned.process->hProcess) {
        AssignProcessToJobObject(job_handle_, spawned.process->hProcess);
      }
#endif
      auto now = std::chrono::steady_clock::now();
      worker.rpc_channel = std::move(spawned.rpc_channel);
      worker.process = std::move(spawned.process);
      worker.stdio_drain_thread = std::move(spawned.stdio_drain_thread);
      worker.pid = spawned_pid;
      worker.idle_start_time = now;
      worker.task_start_time = now;
      exec_start_time = now;
      chan = worker.rpc_channel;
    }
  }

  ParseTaskResult rpc_res;
  bool rpc_success = false;
  if (chan) {
    rpc_res = chan->Call(task, effective_timeout_ms);
    rpc_success = true;
  }

  auto finish_time = std::chrono::steady_clock::now();
  double elapsed_ms = std::chrono::duration<double, std::milli>(finish_time - exec_start_time).count();

  DetachedWorkerResources to_cleanup;
  bool slot_held_for_cleanup = false;
  bool need_release_slot = false;
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (!is_running_.load(std::memory_order_acquire) || pool_generation_ != generation ||
        assigned_worker_idx >= workers_.size()) {
      out_result->status = TaskStatus::kCrashed;
      out_result->error_message = "WorkerGuard shut down during task execution";
      out_result->elapsed_ms = elapsed_ms;
      return false;
    }
    WorkerInstance &worker = *workers_[assigned_worker_idx];

    if (worker.state == WorkerState::kKilledByWatchdog || rpc_res.status == TaskStatus::kTimeout) {
      out_result->status = TaskStatus::kTimeout;
      out_result->error_message =
          rpc_res.error_message.empty() ? "Task timed out or stalled (killed by watchdog)" : rpc_res.error_message;
      to_cleanup = worker.Detach(WorkerState::kRecycling);
      slot_held_for_cleanup = true;
      need_release_slot = !worker.watchdog_cleanup_pending;
    } else if (!rpc_success || rpc_res.status == TaskStatus::kCrashed) {
      out_result->status = TaskStatus::kCrashed;
      out_result->error_message = BuildCrashErrorMessage(
          assigned_worker_idx, rpc_res.error_message, "Worker crashed (SIGSEGV/abort) during file parsing");
      to_cleanup = worker.Detach(WorkerState::kRecycling);
      slot_held_for_cleanup = true;
      need_release_slot = !worker.watchdog_cleanup_pending;
    } else {
      *out_result = std::move(rpc_res);
      worker.idle_start_time = finish_time;
      ++worker.tasks_since_mem_check;

      const size_t max_mem = max_memory_bytes_.load(std::memory_order_relaxed);
      const size_t check_interval = memory_check_interval_tasks_.load(std::memory_order_relaxed);
      const bool heavy_task = (out_result->extracted_bytes >= 64 * 1024 || elapsed_ms >= 50.0);
      bool recycled = false;
      if (max_mem > 0 && worker.pid > 0 && (heavy_task || worker.tasks_since_mem_check >= check_interval)) {
        worker.tasks_since_mem_check = 0;
        const size_t rss = GetProcessRssBytes(worker.pid);
        if (rss > max_mem) {
          LOG_WARN(
              "[WorkerGuard] Worker {} (pid {}) exceeded memory limit at task boundary ({:.1f} MiB > {:.1f} MiB); "
              "recycling",
              assigned_worker_idx, worker.pid, rss / (1024.0 * 1024.0), max_mem / (1024.0 * 1024.0));
          memory_recycle_count_.fetch_add(1, std::memory_order_relaxed);
          to_cleanup = worker.Detach(WorkerState::kRecycling);
          recycled = true;
          slot_held_for_cleanup = true;
          need_release_slot = true;
        }
      }
      if (!recycled) {
        if (worker.watchdog_cleanup_pending) {
          worker.state = WorkerState::kRecycling;
          slot_held_for_cleanup = true;
        } else {
          worker.state = WorkerState::kIdle;
        }
      }
    }
    out_result->elapsed_ms = elapsed_ms;
    if (!slot_held_for_cleanup) {
      worker_available_cv_.notify_all();
    }
  }

  if (need_release_slot) {
    to_cleanup.Reset();
    ReleaseRecycledSlots(&assigned_worker_idx, 1, generation);
  }

  return true;
}

bool WorkerGuard::ExecuteArchiveStream(const ArchiveExtractTask &task, const ArchiveItemCallback &on_item,
                                       ArchiveExtractResult *out_result) {
  if (!out_result) {
    return false;
  }
  out_result->status = TaskStatus::kFailed;
  out_result->error_message.clear();

  uint64_t generation = 0;
  std::shared_ptr<WorkerRpcChannel> chan;
  bool need_spawn = false;
  std::vector<std::string> spawn_args;
  bool spawn_log_as_info = false;
  const uint32_t effective_idle_timeout_ms = task.idle_heartbeat_timeout_ms > 0
                                                 ? task.idle_heartbeat_timeout_ms
                                                 : timeout_ms_.load(std::memory_order_relaxed);
  auto exec_start_time = std::chrono::steady_clock::now();

  {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    worker_available_cv_.wait(lock, [this]() {
      return !is_running_.load(std::memory_order_acquire) ||
             (unpacker_worker_ && unpacker_worker_->state == WorkerState::kIdle);
    });
    if (!is_running_.load(std::memory_order_acquire) || !unpacker_worker_) {
      out_result->status = TaskStatus::kCrashed;
      out_result->error_message = "WorkerGuard is not running";
      return false;
    }

    generation = pool_generation_;
    WorkerInstance &unpacker = *unpacker_worker_;
    unpacker.state = WorkerState::kBusy;
    unpacker.current_timeout_ms = effective_idle_timeout_ms;
    unpacker.task_start_time = exec_start_time;
    unpacker_last_activity_ms_.store(SteadyEpochMs(exec_start_time), std::memory_order_release);
    unpacker_in_callback_.store(false, std::memory_order_release);

    need_spawn = (unpacker.pid <= 0 || !unpacker.rpc_channel);
    if (need_spawn) {
      spawn_args = BuildWorkerArgs("", kUnpackerWorkerIndex);
      spawn_log_as_info = log_console_;
    } else {
      chan = unpacker.rpc_channel;
    }
  }

  if (need_spawn) {
    DetachedWorkerResources spawned;
    WorkerPid spawned_pid = 0;
    bool ok = SpawnWorkerProcess(kUnpackerWorkerIndex, &spawned, &spawned_pid, spawn_args, spawn_log_as_info);
    if (!ok) {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      const bool same_pool = is_running_.load(std::memory_order_acquire) && pool_generation_ == generation;
      if (same_pool && unpacker_worker_) {
        unpacker_worker_->state = WorkerState::kIdle;
      }
      out_result->status = TaskStatus::kFailed;
      out_result->error_message = "Failed to spawn Unpacker worker on demand";
      worker_available_cv_.notify_all();
      return same_pool;
    }

    DetachedWorkerResources stale_resources;
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      if (!is_running_.load(std::memory_order_acquire) || pool_generation_ != generation || !unpacker_worker_) {
        return false;
      }
      WorkerInstance &unpacker = *unpacker_worker_;
      if (unpacker.HasResources()) {
        stale_resources = unpacker.Detach(WorkerState::kBusy);
      }
#ifdef _WIN32
      if (job_handle_ && spawned.process && spawned.process->hProcess) {
        AssignProcessToJobObject(job_handle_, spawned.process->hProcess);
      }
#endif
      auto now = std::chrono::steady_clock::now();
      unpacker.rpc_channel = std::move(spawned.rpc_channel);
      unpacker.process = std::move(spawned.process);
      unpacker.stdio_drain_thread = std::move(spawned.stdio_drain_thread);
      unpacker.pid = spawned_pid;
      unpacker.idle_start_time = now;
      unpacker.task_start_time = now;
      unpacker_last_activity_ms_.store(SteadyEpochMs(now), std::memory_order_release);
      chan = unpacker.rpc_channel;
    }
  }

  ArchiveExtractResult rpc_res;
  bool rpc_success = false;
  if (chan) {
    auto on_activity = [this]() {
      unpacker_last_activity_ms_.store(SteadyEpochMs(std::chrono::steady_clock::now()), std::memory_order_release);
    };
    ArchiveItemCallback guarded_on_item = [this, &on_item](ArchiveStreamItem &&item) -> bool {
      unpacker_in_callback_.store(true, std::memory_order_release);
      bool keep = on_item ? on_item(std::move(item)) : true;
      unpacker_last_activity_ms_.store(SteadyEpochMs(std::chrono::steady_clock::now()), std::memory_order_release);
      unpacker_in_callback_.store(false, std::memory_order_release);
      return keep;
    };
    ArchiveExtractTask effective_task = task;
    effective_task.idle_heartbeat_timeout_ms = effective_idle_timeout_ms;
    rpc_res = chan->CallArchiveStream(effective_task, guarded_on_item, on_activity);
    rpc_success = true;
  }

  auto finish_time = std::chrono::steady_clock::now();
  DetachedWorkerResources to_cleanup;
  bool slot_held_for_cleanup = false;
  bool need_release_slot = false;
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (!is_running_.load(std::memory_order_acquire) || pool_generation_ != generation || !unpacker_worker_) {
      out_result->status = TaskStatus::kCrashed;
      out_result->error_message = "WorkerGuard shut down during archive extraction";
      return false;
    }
    WorkerInstance &unpacker = *unpacker_worker_;

    if (unpacker.state == WorkerState::kKilledByWatchdog || rpc_res.status == TaskStatus::kTimeout) {
      out_result->status = TaskStatus::kTimeout;
      out_result->error_message =
          rpc_res.error_message.empty() ? "Archive Unpacker timed out (stalled)" : rpc_res.error_message;
      to_cleanup = unpacker.Detach(WorkerState::kRecycling);
      slot_held_for_cleanup = true;
      need_release_slot = !unpacker.watchdog_cleanup_pending;
    } else if (!rpc_success || rpc_res.status == TaskStatus::kCrashed) {
      out_result->status = TaskStatus::kCrashed;
      out_result->error_message = BuildCrashErrorMessage(
          kUnpackerWorkerIndex, rpc_res.error_message, "Unpacker crashed during archive extraction");
      to_cleanup = unpacker.Detach(WorkerState::kRecycling);
      slot_held_for_cleanup = true;
      need_release_slot = !unpacker.watchdog_cleanup_pending;
    } else {
      *out_result = std::move(rpc_res);
      unpacker.idle_start_time = finish_time;
      const size_t max_mem = max_memory_bytes_.load(std::memory_order_relaxed);
      bool recycled = false;
      if (max_mem > 0 && unpacker.pid > 0) {
        const size_t rss = GetProcessRssBytes(unpacker.pid);
        if (rss > max_mem) {
          LOG_WARN("[WorkerGuard] Unpacker (pid {}) exceeded memory limit ({:.1f} MiB > {:.1f} MiB); recycling",
                   unpacker.pid, rss / (1024.0 * 1024.0), max_mem / (1024.0 * 1024.0));
          memory_recycle_count_.fetch_add(1, std::memory_order_relaxed);
          to_cleanup = unpacker.Detach(WorkerState::kRecycling);
          recycled = true;
          slot_held_for_cleanup = true;
          need_release_slot = true;
        }
      }
      if (!recycled) {
        if (unpacker.watchdog_cleanup_pending) {
          unpacker.state = WorkerState::kRecycling;
          slot_held_for_cleanup = true;
        } else {
          unpacker.state = WorkerState::kIdle;
        }
      }
    }
    if (!slot_held_for_cleanup) {
      worker_available_cv_.notify_all();
    }
  }

  if (need_release_slot) {
    to_cleanup.Reset();
    const size_t unpacker_slot = kUnpackerWorkerIndex;
    ReleaseRecycledSlots(&unpacker_slot, 1, generation);
  }
  return true;
}

void WorkerGuard::Shutdown() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  ShutdownLocked();
}

void WorkerGuard::ShutdownLocked() {
  is_running_.store(false, std::memory_order_release);

  {
    std::lock_guard<std::mutex> cv_lock(watchdog_mutex_);
  }
  watchdog_cv_.notify_all();
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
  }
  worker_available_cv_.notify_all();

  if (watchdog_thread_.joinable()) {
    watchdog_thread_.join();
  }

  std::vector<std::unique_ptr<WorkerInstance>> old_workers;
  std::unique_ptr<WorkerInstance> old_unpacker;
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    pool_generation_++;
    target_worker_count_ = 0;
    old_workers = std::move(workers_);
    old_unpacker = std::move(unpacker_worker_);
#ifdef _WIN32
    if (job_handle_) {
      CloseHandle(job_handle_);
      job_handle_ = nullptr;
    }
#endif
  }
  worker_available_cv_.notify_all();
  old_workers.clear();
  old_unpacker.reset();
}

void WorkerGuard::SetLoggingOptions(const WorkerLoggingOptions &options) {
  std::vector<DetachedWorkerResources> to_cleanup;
  std::vector<size_t> recycled_indices;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    generation = pool_generation_;
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
      for (size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i]->state == WorkerState::kIdle && workers_[i]->HasResources()) {
          LOG_WARN("[WorkerGuard] Recycling idle worker {} due to logging options change", i);
          to_cleanup.push_back(workers_[i]->Detach(WorkerState::kRecycling));
          recycled_indices.push_back(i);
        }
      }
      if (unpacker_worker_ && unpacker_worker_->state == WorkerState::kIdle && unpacker_worker_->HasResources()) {
        LOG_WARN("[WorkerGuard] Recycling idle Unpacker due to logging options change");
        to_cleanup.push_back(unpacker_worker_->Detach(WorkerState::kRecycling));
        recycled_indices.push_back(kUnpackerWorkerIndex);
      }
    }
  }
  to_cleanup.clear();
  ReleaseRecycledSlots(recycled_indices.data(), recycled_indices.size(), generation);
}

void WorkerGuard::SetWorkerCount(size_t new_count) {
  if (new_count == 0) {
    new_count = 1;
  }

  std::vector<DetachedWorkerResources> to_cleanup;
  {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    if (!is_running_.load(std::memory_order_acquire)) {
      return;
    }
    const uint64_t generation = pool_generation_;
    size_t current_count = target_worker_count_ > 0 ? target_worker_count_ : workers_.size();
    if (new_count == current_count && workers_.size() == new_count) {
      return;
    }

    target_worker_count_ = new_count;

    if (new_count > workers_.size()) {
      auto now = std::chrono::steady_clock::now();
      workers_.reserve(new_count);
      while (workers_.size() < new_count) {
        auto inst = std::make_unique<WorkerInstance>();
        inst->idle_start_time = now;
        workers_.push_back(std::move(inst));
      }
      {
        std::lock_guard<std::mutex> stdio_lock(stdio_mutex_);
        recent_stdio_.resize(new_count);
      }
      LOG_INFO("[WorkerGuard] Dynamically scaled UP worker pool from {} to {}", current_count, new_count);
      worker_available_cv_.notify_all();
    } else {
      if (new_count > current_count) {
        LOG_INFO("[WorkerGuard] Dynamically scaling UP worker pool from {} to {} (draining excess slots)",
                 current_count, new_count);
        worker_available_cv_.notify_all();
      } else {
        LOG_INFO("[WorkerGuard] Dynamically scaling DOWN worker pool from {} to {}", current_count, new_count);
      }

      std::vector<DetachedWorkerResources> idle_cleanup;
      std::vector<size_t> idle_recycled_indices;
      for (size_t i = new_count; i < workers_.size(); ++i) {
        if (workers_[i]->state == WorkerState::kIdle && workers_[i]->HasResources()) {
          idle_cleanup.push_back(workers_[i]->Detach(WorkerState::kRecycling));
          idle_recycled_indices.push_back(i);
        }
      }
      if (!idle_cleanup.empty()) {
        lock.unlock();
        idle_cleanup.clear();
        ReleaseRecycledSlots(idle_recycled_indices.data(), idle_recycled_indices.size(), generation);
        lock.lock();
      }

      while (true) {
        if (!is_running_.load(std::memory_order_acquire) || pool_generation_ != generation ||
            target_worker_count_ != new_count) {
          return;
        }
        bool any_busy = false;
        for (size_t i = new_count; i < workers_.size(); ++i) {
          if (workers_[i]->state != WorkerState::kIdle) {
            any_busy = true;
            break;
          }
        }
        if (!any_busy) {
          break;
        }
        worker_available_cv_.wait_for(lock, std::chrono::milliseconds(10));
      }
      if (!is_running_.load(std::memory_order_acquire) || pool_generation_ != generation ||
          target_worker_count_ != new_count) {
        return;
      }
      for (size_t i = new_count; i < workers_.size(); ++i) {
        if (workers_[i]->HasResources()) {
          to_cleanup.push_back(workers_[i]->Detach(WorkerState::kIdle));
        }
      }
      if (new_count < workers_.size()) {
        workers_.resize(new_count);
      }
      {
        std::lock_guard<std::mutex> stdio_lock(stdio_mutex_);
        recent_stdio_.resize(new_count);
      }
      LOG_INFO("[WorkerGuard] Worker pool successfully adjusted to {}", new_count);
      worker_available_cv_.notify_all();
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
