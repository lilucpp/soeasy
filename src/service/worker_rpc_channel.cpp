#include "service/worker_rpc_channel.h"

#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>

#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "service/worker_guard.h"
#include "soeasy_ipc.capnp.h"

namespace soeasy {
namespace service {

namespace {

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
      switch (p.getStatusCode()) {
        case soeasy::ipc::ParseStatusCode::OK:
          out_->status = TaskStatus::kSuccess;
          break;
        case soeasy::ipc::ParseStatusCode::ENCRYPTED:
          out_->status = TaskStatus::kEncrypted;
          break;
        case soeasy::ipc::ParseStatusCode::TIMEOUT:
          out_->status = TaskStatus::kTimeout;
          break;
        case soeasy::ipc::ParseStatusCode::CRASHED:
          out_->status = TaskStatus::kCrashed;
          break;
        default:
          out_->status = TaskStatus::kFailed;
          break;
      }
      out_->error_message = p.getErrorMsg().cStr();
    }
    return kj::READY_NOW;
  }

 private:
  std::shared_ptr<ParseTaskResult> out_;
};

}  // namespace

struct WorkerRpcChannel::TaskRequest {
  ParseTask task;
  uint32_t timeout_ms = WorkerGuard::kDefaultTimeoutMs;
  std::promise<ParseTaskResult> done_promise;
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
  auto req = std::make_shared<TaskRequest>();
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
        std::shared_ptr<TaskRequest> req;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          cv_.wait(lock, [this]() { return stopped_ || !queue_.empty(); });
          if (stopped_) {
            break;
          }
          req = queue_.front();
          queue_.pop_front();
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

          auto rpcReq = agent.parseFileStreamRequest();
          rpcReq.setDocId(req->task.doc_id);
          rpcReq.setFilePath(req->task.file_path);
          rpcReq.setExt(req->task.ext);
          rpcReq.setLevel(static_cast<soeasy::ipc::ParseLevel>(req->task.parse_level));
          rpcReq.setReceiver(kj::heap<TextStreamReceiverImpl>(result));

          auto sendPromise = rpcReq.send().ignoreResult();
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
      ParseTaskResult res;
      res.doc_id = pending->task.doc_id;
      res.status = TaskStatus::kCrashed;
      res.error_message = "Worker RPC channel terminated";
      pending->done_promise.set_value(std::move(res));
    }
  }
  cv_.notify_all();
}

}  // namespace service
}  // namespace soeasy
