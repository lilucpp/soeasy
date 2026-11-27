#include "service/worker_rpc_channel.h"

#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "service/worker_guard.h"
#include "soeasy_ipc.capnp.h"

namespace soeasy {
namespace service {

namespace {

int64_t SteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

TaskStatus MapIpcStatusCode(soeasy::ipc::ParseStatusCode code) {
  switch (code) {
    case soeasy::ipc::ParseStatusCode::OK:
      return TaskStatus::kSuccess;
    case soeasy::ipc::ParseStatusCode::PARTIAL_OCR:
      return TaskStatus::kPartialOcr;
    case soeasy::ipc::ParseStatusCode::EMPTY_CONTENT:
      return TaskStatus::kEmptyContent;
    case soeasy::ipc::ParseStatusCode::ENCRYPTED:
      return TaskStatus::kEncrypted;
    case soeasy::ipc::ParseStatusCode::TIMEOUT:
      return TaskStatus::kTimeout;
    case soeasy::ipc::ParseStatusCode::CRASHED:
      return TaskStatus::kCrashed;
    default:
      return TaskStatus::kFailed;
  }
}

ArchiveStreamEvent MapIpcArchiveEvent(soeasy::ipc::ArchiveItemEvent event) {
  switch (event) {
    case soeasy::ipc::ArchiveItemEvent::DATA:
      return ArchiveStreamEvent::kData;
    case soeasy::ipc::ArchiveItemEvent::ENCRYPTED:
      return ArchiveStreamEvent::kEncrypted;
    case soeasy::ipc::ArchiveItemEvent::SKIPPED_TOO_LARGE:
      return ArchiveStreamEvent::kSkippedTooLarge;
    case soeasy::ipc::ArchiveItemEvent::HEARTBEAT:
      return ArchiveStreamEvent::kHeartbeat;
    default:
      return ArchiveStreamEvent::kFailed;
  }
}

class TextStreamReceiverImpl final : public soeasy::ipc::TextStreamReceiver::Server {
 public:
  explicit TextStreamReceiverImpl(std::shared_ptr<ParseTaskResult> out) : out_(std::move(out)) {}

  kj::Promise<void> sendChunk(SendChunkContext context) override {
    auto chunk = context.getParams().getChunk();
    if (out_ && chunk.size() > 0) {
      out_->extracted_text.append(reinterpret_cast<const char *>(chunk.begin()), chunk.size());
      out_->stream_chunk_count++;
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> finish(FinishContext context) override {
    auto p = context.getParams();
    if (out_) {
      out_->extracted_bytes = p.getTotalBytes();
      out_->is_scanned_pdf = p.getIsScannedPdf();
      out_->status = MapIpcStatusCode(p.getStatusCode());
      out_->error_message = p.getErrorMsg().cStr();
    }
    return kj::READY_NOW;
  }

 private:
  std::shared_ptr<ParseTaskResult> out_;
};

class ArchiveStreamReceiverImpl final : public soeasy::ipc::ArchiveStreamReceiver::Server {
 public:
  ArchiveStreamReceiverImpl(ArchiveItemCallback on_item, std::function<void()> on_activity,
                            std::shared_ptr<std::atomic<int64_t>> last_activity_ms,
                            std::shared_ptr<std::atomic<bool>> in_callback,
                            std::shared_ptr<ArchiveExtractResult> out)
      : on_item_(std::move(on_item)),
        on_activity_(std::move(on_activity)),
        last_activity_ms_(std::move(last_activity_ms)),
        in_callback_(std::move(in_callback)),
        out_(std::move(out)) {}

  kj::Promise<void> onItemMeta(OnItemMetaContext context) override {
    TouchActivity();

    auto p = context.getParams();
    const ArchiveStreamEvent ev = MapIpcArchiveEvent(p.getEvent());
    if (ev == ArchiveStreamEvent::kHeartbeat) {
      context.getResults().setKeepGoing(true);
      return kj::READY_NOW;
    }

    ArchiveStreamItem item;
    item.virtual_path = p.getVirtualPath().cStr();
    item.file_name = p.getFileName().cStr();
    item.ext = p.getExt().cStr();
    item.entry_size = p.getEntrySize();
    item.entry_mtime = p.getEntryMtime();
    item.event = ev;
    item.message = p.getMessage().cStr();

    if (ev != ArchiveStreamEvent::kData) {
      has_pending_ = false;
      pending_item_ = ArchiveStreamItem{};
      context.getResults().setKeepGoing(DispatchItem(std::move(item)));
      return kj::READY_NOW;
    }

    pending_item_ = std::move(item);
    constexpr uint64_t kMaxPreReserveBytes = 16ULL * 1024ULL * 1024ULL;
    if (pending_item_.entry_size > 0) {
      pending_item_.raw_bytes.reserve(
          static_cast<size_t>(std::min<uint64_t>(pending_item_.entry_size, kMaxPreReserveBytes)));
    }
    has_pending_ = true;
    context.getResults().setKeepGoing(true);
    return kj::READY_NOW;
  }

  kj::Promise<void> onItemChunk(OnItemChunkContext context) override {
    TouchActivity();
    if (has_pending_) {
      auto chunk = context.getParams().getChunk();
      if (chunk.size() > 0) {
        pending_item_.raw_bytes.append(reinterpret_cast<const char *>(chunk.begin()), chunk.size());
      }
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> onItemEnd(OnItemEndContext context) override {
    TouchActivity();
    if (!has_pending_) {
      context.getResults().setKeepGoing(true);
      return kj::READY_NOW;
    }
    has_pending_ = false;
    ArchiveStreamItem completed = std::move(pending_item_);
    pending_item_ = ArchiveStreamItem{};
    context.getResults().setKeepGoing(DispatchItem(std::move(completed)));
    return kj::READY_NOW;
  }

  kj::Promise<void> finish(FinishContext context) override {
    auto p = context.getParams();
    if (out_) {
      out_->status = MapIpcStatusCode(p.getStatusCode());
      out_->error_message = p.getMessage().cStr();
    }
    return kj::READY_NOW;
  }

 private:
  void TouchActivity() {
    if (last_activity_ms_) {
      last_activity_ms_->store(SteadyNowMs(), std::memory_order_release);
    }
    if (on_activity_) {
      on_activity_();
    }
  }

  bool DispatchItem(ArchiveStreamItem &&item) {
    bool keep = true;
    if (on_item_) {
      if (in_callback_) {
        in_callback_->store(true, std::memory_order_release);
      }
      keep = on_item_(std::move(item));
      if (last_activity_ms_) {
        last_activity_ms_->store(SteadyNowMs(), std::memory_order_release);
      }
      if (in_callback_) {
        in_callback_->store(false, std::memory_order_release);
      }
      if (on_activity_) {
        on_activity_();
      }
    }
    return keep;
  }

  ArchiveItemCallback on_item_;
  std::function<void()> on_activity_;
  std::shared_ptr<std::atomic<int64_t>> last_activity_ms_;
  std::shared_ptr<std::atomic<bool>> in_callback_;
  std::shared_ptr<ArchiveExtractResult> out_;
  bool has_pending_ = false;
  ArchiveStreamItem pending_item_;
};

kj::Promise<void> MakeArchiveIdleWatchdog(kj::Timer &timer, uint32_t idle_timeout_ms,
                                          const std::shared_ptr<std::atomic<int64_t>> &last_activity_ms,
                                          const std::shared_ptr<std::atomic<bool>> &in_callback,
                                          const std::shared_ptr<bool> &timed_out) {
  return timer.afterDelay(1000 * kj::MILLISECONDS)
      .then([&timer, idle_timeout_ms, last_activity_ms, in_callback, timed_out]() -> kj::Promise<void> {
        if (!in_callback->load(std::memory_order_acquire)) {
          const int64_t idle_ms = SteadyNowMs() - last_activity_ms->load(std::memory_order_acquire);
          if (idle_ms > static_cast<int64_t>(idle_timeout_ms)) {
            *timed_out = true;
            KJ_FAIL_REQUIRE("Archive Unpacker inactivity timeout");
          }
        }
        return MakeArchiveIdleWatchdog(timer, idle_timeout_ms, last_activity_ms, in_callback, timed_out);
      });
}

}  // namespace

struct WorkerRpcChannel::RequestItem {
  bool is_archive = false;
  ParseTask task;
  uint32_t timeout_ms = WorkerGuard::kDefaultTimeoutMs;
  std::promise<ParseTaskResult> done_promise;

  ArchiveExtractTask archive_task;
  ArchiveItemCallback on_item;
  std::function<void()> on_activity;
  std::promise<ArchiveExtractResult> archive_done_promise;
};

WorkerRpcChannel::WorkerRpcChannel(uint32_t accept_timeout_ms)
    : accept_timeout_ms_(accept_timeout_ms > 0 ? accept_timeout_ms : 5000) {
  auto port_fut = port_promise_.get_future();
  thread_ = std::thread([this]() { Loop(); });
  listening_port_ = port_fut.get();
}

WorkerRpcChannel::~WorkerRpcChannel() {
  Stop();
}

bool WorkerRpcChannel::WaitConnected(uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this]() { return connected_ || stopped_; }) &&
         connected_ && !stopped_;
}

ParseTaskResult WorkerRpcChannel::Call(const ParseTask &task, uint32_t timeout_ms) {
  auto req = std::make_shared<RequestItem>();
  req->is_archive = false;
  req->task = task;
  req->timeout_ms = timeout_ms > 0 ? timeout_ms : WorkerGuard::kDefaultTimeoutMs;
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

ArchiveExtractResult WorkerRpcChannel::CallArchiveStream(const ArchiveExtractTask &task,
                                                         const ArchiveItemCallback &on_item,
                                                         const std::function<void()> &on_activity) {
  auto req = std::make_shared<RequestItem>();
  req->is_archive = true;
  req->archive_task = task;
  req->on_item = on_item;
  req->on_activity = on_activity;
  auto fut = req->archive_done_promise.get_future();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      ArchiveExtractResult res;
      res.status = TaskStatus::kCrashed;
      res.error_message = "Worker RPC channel stopped";
      return res;
    }
    queue_.push_back(req);
  }
  cv_.notify_one();
  return fut.get();
}

void WorkerRpcChannel::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    KJ_IF_MAYBE(f, active_fulfiller_) {
      (*f)->fulfill();
    }
    active_fulfiller_ = nullptr;
  }
  cv_.notify_all();
  std::lock_guard<std::mutex> join_lock(join_mutex_);
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
    thread_.join();
  }
}

void WorkerRpcChannel::Loop() {
  std::unique_ptr<kj::AsyncIoContext> ioContextPtr;
  kj::Own<kj::AsyncIoStream> clientStream;
  bool port_published = false;

  auto clear_fulfiller = [this]() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_fulfiller_ = nullptr;
  };

  try {
    ioContextPtr = std::make_unique<kj::AsyncIoContext>(kj::setupAsyncIo());
    auto &ioContext = *ioContextPtr;
    auto addr = ioContext.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(ioContext.waitScope);
    auto listener = addr->listen();
    port_promise_.set_value(static_cast<uint16_t>(listener->getPort()));
    port_published = true;

    bool already_stopped = false;
    auto cancelPaf = kj::newPromiseAndCrossThreadFulfiller<void>();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) {
        already_stopped = true;
      } else {
        active_fulfiller_ = kj::mv(cancelPaf.fulfiller);
      }
    }

    if (!already_stopped) {
      auto cancelAccept = cancelPaf.promise.then([]() -> kj::Own<kj::AsyncIoStream> {
        KJ_FAIL_REQUIRE("Worker RPC accept cancelled");
      });
      auto acceptTimed =
          ioContext.provider->getTimer().timeoutAfter(accept_timeout_ms_ * kj::MILLISECONDS, listener->accept());
      clientStream = acceptTimed.exclusiveJoin(kj::mv(cancelAccept)).wait(ioContext.waitScope);
      listener = nullptr;

      {
        std::lock_guard<std::mutex> lock(mutex_);
        active_fulfiller_ = nullptr;
        connected_ = true;
      }
      cv_.notify_all();

      capnp::TwoPartyClient client(*clientStream);
      auto agent = client.bootstrap().castAs<soeasy::ipc::WorkerAgent>();

      while (true) {
        std::shared_ptr<RequestItem> req;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          cv_.wait(lock, [this]() { return stopped_ || !queue_.empty(); });
          if (stopped_) {
            break;
          }
          req = queue_.front();
          queue_.pop_front();
        }

        if (req->is_archive) {
          auto archive_result = std::make_shared<ArchiveExtractResult>();
          auto timed_out = std::make_shared<bool>(false);
          try {
            auto taskCancelPaf = kj::newPromiseAndCrossThreadFulfiller<void>();
            {
              std::lock_guard<std::mutex> lock(mutex_);
              if (stopped_) {
                archive_result->status = TaskStatus::kCrashed;
                archive_result->error_message = "Worker RPC channel stopped";
                req->archive_done_promise.set_value(std::move(*archive_result));
                break;
              }
              active_fulfiller_ = kj::mv(taskCancelPaf.fulfiller);
            }

            auto last_activity_ms = std::make_shared<std::atomic<int64_t>>(SteadyNowMs());
            auto in_callback = std::make_shared<std::atomic<bool>>(false);
            const uint32_t idle_timeout_ms = req->archive_task.idle_heartbeat_timeout_ms > 0
                                                 ? req->archive_task.idle_heartbeat_timeout_ms
                                                 : WorkerGuard::kDefaultTimeoutMs;

            auto rpcReq = agent.extractArchiveStreamRequest();
            rpcReq.setArchivePath(req->archive_task.archive_path);
            auto exts = rpcReq.initEnabledExts(static_cast<unsigned int>(req->archive_task.enabled_exts.size()));
            for (unsigned int i = 0; i < req->archive_task.enabled_exts.size(); ++i) {
              exts.set(i, req->archive_task.enabled_exts[i]);
            }
            rpcReq.setMaxEntryBytes(req->archive_task.max_entry_bytes);
            rpcReq.setReceiver(kj::heap<ArchiveStreamReceiverImpl>(req->on_item, req->on_activity, last_activity_ms,
                                                                   in_callback, archive_result));

            auto sendPromise = rpcReq.send().ignoreResult();
            auto cancelTask = taskCancelPaf.promise.then([]() { KJ_FAIL_REQUIRE("Archive RPC call cancelled"); });
            auto timeoutTask = MakeArchiveIdleWatchdog(ioContext.provider->getTimer(), idle_timeout_ms,
                                                       last_activity_ms, in_callback, timed_out);
            kj::mv(sendPromise)
                .exclusiveJoin(kj::mv(timeoutTask))
                .exclusiveJoin(kj::mv(cancelTask))
                .wait(ioContext.waitScope);

            clear_fulfiller();
            req->archive_done_promise.set_value(std::move(*archive_result));
          } catch (const kj::Exception &e) {
            clear_fulfiller();
            std::string desc = e.getDescription().cStr();
            if (*timed_out) {
              archive_result->status = TaskStatus::kTimeout;
              archive_result->error_message = "Archive extraction timed out: " + desc;
            } else {
              archive_result->status = TaskStatus::kCrashed;
              archive_result->error_message = "Unpacker peer disconnected: " + desc;
            }
            req->archive_done_promise.set_value(std::move(*archive_result));
            break;
          } catch (const std::exception &e) {
            clear_fulfiller();
            archive_result->status = TaskStatus::kCrashed;
            archive_result->error_message = std::string("Unpacker exception: ") + e.what();
            req->archive_done_promise.set_value(std::move(*archive_result));
            break;
          } catch (...) {
            clear_fulfiller();
            archive_result->status = TaskStatus::kCrashed;
            archive_result->error_message = "Unpacker unknown exception";
            req->archive_done_promise.set_value(std::move(*archive_result));
            break;
          }
          continue;
        }

        auto result = std::make_shared<ParseTaskResult>();
        result->doc_id = req->task.doc_id;

        bool timed_out = false;
        try {
          auto taskCancelPaf = kj::newPromiseAndCrossThreadFulfiller<void>();
          {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
              result->status = TaskStatus::kCrashed;
              result->error_message = "Worker RPC channel stopped";
              req->done_promise.set_value(std::move(*result));
              break;
            }
            active_fulfiller_ = kj::mv(taskCancelPaf.fulfiller);
          }

          kj::Promise<void> sendPromise = kj::READY_NOW;
          if (req->task.raw_bytes != nullptr) {
            auto rpcReq = agent.beginBufferParseRequest();
            rpcReq.setDocId(req->task.doc_id);
            rpcReq.setFilePath(req->task.file_path);
            rpcReq.setExt(req->task.ext);
            rpcReq.setTotalBytes(req->task.raw_bytes->size());
            rpcReq.setLevel(static_cast<soeasy::ipc::ParseLevel>(req->task.parse_level));
            rpcReq.setTimeoutMs(req->timeout_ms);
            auto bytes = req->task.raw_bytes;
            sendPromise = rpcReq.send().then([bytes, result](auto resp) mutable {
              auto session = resp.getSession();
              constexpr size_t kBufferChunkBytes = 1024ULL * 1024ULL;  // 1 MiB chunk << 64 MiB RPC limit
              kj::Promise<void> chunkChain = kj::READY_NOW;
              for (size_t offset = 0; offset < bytes->size(); offset += kBufferChunkBytes) {
                const size_t len = std::min(kBufferChunkBytes, bytes->size() - offset);
                chunkChain = chunkChain.then([session, bytes, offset, len]() mutable {
                  auto creq = session.sendChunkRequest();
                  creq.setChunk(kj::ArrayPtr<const kj::byte>(
                      reinterpret_cast<const kj::byte *>(bytes->data() + offset), len));
                  return creq.send().ignoreResult();
                });
              }
              bytes.reset();
              return chunkChain.then([session, result]() mutable {
                auto freq = session.finishRequest();
                freq.setReceiver(kj::heap<TextStreamReceiverImpl>(result));
                return freq.send().ignoreResult();
              });
            });
          } else {
            auto rpcReq = agent.parseFileStreamRequest();
            rpcReq.setDocId(req->task.doc_id);
            rpcReq.setFilePath(req->task.file_path);
            rpcReq.setExt(req->task.ext);
            rpcReq.setLevel(static_cast<soeasy::ipc::ParseLevel>(req->task.parse_level));
            rpcReq.setReceiver(kj::heap<TextStreamReceiverImpl>(result));
            rpcReq.setTimeoutMs(req->timeout_ms);
            sendPromise = rpcReq.send().ignoreResult();
          }

          auto cancelTask = taskCancelPaf.promise.then([]() { KJ_FAIL_REQUIRE("Worker RPC call cancelled"); });
          auto timeoutTask = ioContext.provider->getTimer()
                                 .afterDelay(req->timeout_ms * kj::MILLISECONDS)
                                 .then([&timed_out]() {
                                   timed_out = true;
                                   KJ_FAIL_REQUIRE("Worker RPC task timed out");
                                 });
          kj::mv(sendPromise)
              .exclusiveJoin(kj::mv(timeoutTask))
              .exclusiveJoin(kj::mv(cancelTask))
              .wait(ioContext.waitScope);

          clear_fulfiller();
          req->done_promise.set_value(std::move(*result));
        } catch (const kj::Exception &e) {
          clear_fulfiller();
          std::string desc = e.getDescription().cStr();
          if (timed_out) {
            result->status = TaskStatus::kTimeout;
            result->error_message = "Task timed out: " + desc;
          } else {
            result->status = TaskStatus::kCrashed;
            result->error_message = "Worker peer disconnected: " + desc;
          }
          req->done_promise.set_value(std::move(*result));
          break;
        } catch (const std::exception &e) {
          clear_fulfiller();
          result->status = TaskStatus::kCrashed;
          result->error_message = std::string("Worker exception: ") + e.what();
          req->done_promise.set_value(std::move(*result));
          break;
        } catch (...) {
          clear_fulfiller();
          result->status = TaskStatus::kCrashed;
          result->error_message = "Worker unknown exception";
          req->done_promise.set_value(std::move(*result));
          break;
        }
      }
    }
  } catch (...) {
    if (!port_published) {
      try {
        port_promise_.set_value(0);
      } catch (...) {
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_fulfiller_ = nullptr;
    stopped_ = true;
    while (!queue_.empty()) {
      auto pending = queue_.front();
      queue_.pop_front();
      if (pending->is_archive) {
        ArchiveExtractResult res;
        res.status = TaskStatus::kCrashed;
        res.error_message = "Worker RPC channel terminated";
        pending->archive_done_promise.set_value(std::move(res));
      } else {
        ParseTaskResult res;
        res.doc_id = pending->task.doc_id;
        res.status = TaskStatus::kCrashed;
        res.error_message = "Worker RPC channel terminated";
        pending->done_promise.set_value(std::move(res));
      }
    }
  }
  cv_.notify_all();
}

}  // namespace service
}  // namespace soeasy
