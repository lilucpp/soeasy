#ifndef SOEASY_SRC_SERVICE_WORKER_RPC_CHANNEL_H_
#define SOEASY_SRC_SERVICE_WORKER_RPC_CHANNEL_H_

#include <kj/async.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace soeasy {
namespace service {

struct ArchiveExtractResult;
struct ArchiveExtractTask;
struct ArchiveStreamItem;
struct ParseTask;
struct ParseTaskResult;
using ArchiveItemCallback = std::function<bool(ArchiveStreamItem &&)>;

class WorkerRpcChannel {
 public:
  explicit WorkerRpcChannel(uint32_t accept_timeout_ms = 5000);
  ~WorkerRpcChannel();

  WorkerRpcChannel(const WorkerRpcChannel &) = delete;
  WorkerRpcChannel &operator=(const WorkerRpcChannel &) = delete;

  uint16_t listening_port() const { return listening_port_; }

  bool WaitConnected(uint32_t timeout_ms);
  ParseTaskResult Call(const ParseTask &task, uint32_t timeout_ms);
  ArchiveExtractResult CallArchiveStream(const ArchiveExtractTask &task, const ArchiveItemCallback &on_item,
                                         const std::function<void()> &on_activity);
  void Stop();

 private:
  struct RequestItem;

  void Loop();

  uint32_t accept_timeout_ms_ = 5000;
  std::promise<uint16_t> port_promise_;
  uint16_t listening_port_ = 0;
  std::mutex mutex_;
  std::mutex join_mutex_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<RequestItem>> queue_;
  kj::Maybe<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> active_fulfiller_;
  bool connected_ = false;
  bool stopped_ = false;
  std::thread thread_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_WORKER_RPC_CHANNEL_H_
