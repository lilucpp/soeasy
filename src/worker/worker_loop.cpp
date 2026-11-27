#include "worker/worker_loop.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/prctl.h>
#include <unistd.h>
#endif

#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/logger.h"
#include "common/system_env.h"
#include "soeasy_ipc.capnp.h"
#include "worker/archive_reader.h"
#include "worker/parser_registry.h"

namespace soeasy {
namespace worker {

namespace {

void CheckTestHooks(const std::string &file_path) {
  if (file_path.find("TEST_TRIGGER_HANG") != std::string::npos) {
    while (true) {
#ifdef _WIN32
      Sleep(100);
#else
      usleep(100000);
#endif
    }
  }
  if (file_path.find("TEST_TRIGGER_CRASH") != std::string::npos) {
#ifdef _WIN32
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
#else
    std::raise(SIGSEGV);
#endif
  }
}

std::pair<soeasy::ipc::ParseStatusCode, std::string> BuildFailureStatusAndLog(const std::string &file_path,
                                                                              const std::string &ext,
                                                                              const ExtractedDocument &doc,
                                                                              double cost_ms) {
  const bool is_encrypted = (doc.outcome == ParseOutcome::kEncrypted);
  const auto status_code = is_encrypted ? soeasy::ipc::ParseStatusCode::ENCRYPTED : soeasy::ipc::ParseStatusCode::FAILED;
  const std::string err_msg =
      doc.error_message.empty()
          ? (is_encrypted ? "Document is encrypted and requires a password" : "Failed to parse document")
          : doc.error_message;
  if (is_encrypted) {
    LOG_WARN("[Worker] Parse ENCRYPTED (stream) | file: '{}' | ext: '{}' | cost: {:.2f} ms | {}", file_path, ext,
             cost_ms, err_msg);
  } else {
    LOG_ERROR("[Worker] Parse FAILED (stream) | file: '{}' | ext: '{}' | cost: {:.2f} ms | {}", file_path, ext,
              cost_ms, err_msg);
  }
  return {status_code, err_msg};
}

void MaybeTrimWorkerHeap(ParserRegistry &registry, const std::shared_ptr<size_t> &tasks_since_trim,
                         size_t extracted_bytes, double cost_ms) {
  const size_t count = tasks_since_trim ? ++(*tasks_since_trim) : 0;
  if (extracted_bytes >= 64 * 1024 || cost_ms >= 15.0 || count >= 32) {
    if (tasks_since_trim) *tasks_since_trim = 0;
    registry.CheckIdleUnload(60.0);
    common::SystemEnv::TrimHeap();
  }
}

kj::Promise<void> StreamDocumentResult(ParserRegistry &registry, std::shared_ptr<size_t> tasks_since_trim,
                                       const std::string &file_path, const std::string &ext, ExtractedDocument &&doc,
                                       double cost_ms, bool parse_ok,
                                       soeasy::ipc::TextStreamReceiver::Client receiver) {
  if (!parse_ok) {
    const auto failure = BuildFailureStatusAndLog(file_path, ext, doc, cost_ms);
    std::string().swap(doc.utf8_text);
    MaybeTrimWorkerHeap(registry, tasks_since_trim, 0, cost_ms);
    auto finishReq = receiver.finishRequest();
    finishReq.setTotalBytes(0);
    finishReq.setIsScannedPdf(false);
    finishReq.setStatusCode(failure.first);
    finishReq.setErrorMsg(failure.second);
    return finishReq.send().then([](auto &&) {});
  }

  constexpr size_t kChunkSize = 256 * 1024;
  size_t text_bytes = doc.utf8_text.size();
  size_t num_chunks = (text_bytes + kChunkSize - 1) / kChunkSize;
  auto textShared = std::make_shared<std::string>(std::move(doc.utf8_text));
  const bool is_scanned = doc.is_scanned_pdf;
  const bool partial_ocr = (doc.outcome == ParseOutcome::kPartialOcr);
  const bool empty_content = (doc.outcome == ParseOutcome::kEmptyContent);
  auto finish_status = soeasy::ipc::ParseStatusCode::OK;
  std::string finish_msg = "Success";
  if (partial_ocr) {
    finish_status = soeasy::ipc::ParseStatusCode::PARTIAL_OCR;
    if (!doc.error_message.empty()) finish_msg = doc.error_message;
    LOG_WARN("[Worker] Parse PARTIAL_OCR (stream) | file: '{}' | ext: '{}' | extracted: {} bytes | cost: {:.2f} ms | {}",
             file_path, ext, text_bytes, cost_ms, finish_msg);
  } else if (empty_content) {
    finish_status = soeasy::ipc::ParseStatusCode::EMPTY_CONTENT;
    finish_msg = !doc.error_message.empty() ? doc.error_message : "Document contains no extractable text";
    text_bytes = 0;
    num_chunks = 0;
    LOG_INFO("[Worker] Parse EMPTY_CONTENT (stream) | file: '{}' | ext: '{}' | cost: {:.2f} ms | {}", file_path, ext,
             cost_ms, finish_msg);
  } else {
    LOG_INFO("[Worker] Parse SUCCESS (stream) | file: '{}' | ext: '{}' | extracted: {} bytes | cost: {:.2f} ms",
             file_path, ext, text_bytes, cost_ms);
  }

  kj::Promise<void> streamPromise = kj::READY_NOW;
  for (size_t i = 0; i < num_chunks; ++i) {
    size_t offset = i * kChunkSize;
    size_t len = std::min(kChunkSize, text_bytes - offset);
    streamPromise = streamPromise.then([receiver, textShared, offset, len]() mutable {
      auto req = receiver.sendChunkRequest();
      req.setChunk(kj::ArrayPtr<const kj::byte>(
          reinterpret_cast<const kj::byte *>(textShared->data() + offset), len));
      textShared.reset();
      return req.send().then([](auto &&) {});
    });
  }
  textShared.reset();

  return streamPromise.then(
      [&registry, tasks_since_trim, receiver, is_scanned, text_bytes, cost_ms, finish_status, finish_msg]() mutable {
        MaybeTrimWorkerHeap(registry, tasks_since_trim, text_bytes, cost_ms);
        auto finishReq = receiver.finishRequest();
        finishReq.setTotalBytes(text_bytes);
        finishReq.setIsScannedPdf(is_scanned);
        finishReq.setStatusCode(finish_status);
        finishReq.setErrorMsg(finish_msg);
        return finishReq.send().then([](auto &&) {});
      });
}

class BufferParseSessionImpl final : public soeasy::ipc::BufferParseSession::Server {
 public:
  BufferParseSessionImpl(ParserRegistry &registry, std::shared_ptr<size_t> tasks_since_trim, int64_t doc_id,
                         std::string file_path, std::string ext, uint64_t total_bytes, uint32_t timeout_ms)
      : registry_(registry),
        tasks_since_trim_(std::move(tasks_since_trim)),
        doc_id_(doc_id),
        file_path_(std::move(file_path)),
        ext_(std::move(ext)),
        timeout_ms_(timeout_ms > 0 ? timeout_ms : 60000) {
    constexpr uint64_t kMaxPreReserveBytes = 16ULL * 1024ULL * 1024ULL;
    if (total_bytes > 0) {
      buffer_.reserve(static_cast<size_t>(std::min<uint64_t>(total_bytes, kMaxPreReserveBytes)));
    }
  }

  kj::Promise<void> sendChunk(SendChunkContext context) override {
    auto chunk = context.getParams().getChunk();
    if (chunk.size() > 0) {
      buffer_.append(reinterpret_cast<const char *>(chunk.begin()), chunk.size());
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> finish(FinishContext context) override {
    CheckTestHooks(file_path_);

    ParseContext ctx;
    ctx.doc_id = doc_id_;
    ctx.file_path = file_path_;
    ctx.ext = ext_;
    ctx.file_size = buffer_.size();
    ctx.timeout_ms = timeout_ms_;

    ExtractedDocument doc;
    auto parse_start = std::chrono::high_resolution_clock::now();
    bool parse_ok = registry_.ParseBufferDocument(buffer_.data(), buffer_.size(), ctx, &doc);
    std::string().swap(buffer_);
    auto parse_end = std::chrono::high_resolution_clock::now();
    double cost_ms = std::chrono::duration<double, std::milli>(parse_end - parse_start).count();

    return StreamDocumentResult(registry_, tasks_since_trim_, file_path_, ext_, std::move(doc), cost_ms, parse_ok,
                                context.getParams().getReceiver());
  }

 private:
  ParserRegistry &registry_;
  std::shared_ptr<size_t> tasks_since_trim_;
  int64_t doc_id_ = 0;
  std::string file_path_;
  std::string ext_;
  uint32_t timeout_ms_ = 60000;
  std::string buffer_;
};

}  // namespace

class WorkerAgentImpl final : public soeasy::ipc::WorkerAgent::Server {
 public:
  explicit WorkerAgentImpl(ParserRegistry &registry)
      : registry_(registry), tasks_since_trim_(std::make_shared<size_t>(0)) {}

  kj::Promise<void> beginBufferParse(BeginBufferParseContext context) override {
    auto params = context.getParams();
    context.getResults().setSession(kj::heap<BufferParseSessionImpl>(
        registry_, tasks_since_trim_, params.getDocId(), params.getFilePath().cStr(), params.getExt().cStr(),
        params.getTotalBytes(), params.getTimeoutMs()));
    return kj::READY_NOW;
  }

  kj::Promise<void> extractArchiveStream(ExtractArchiveStreamContext context) override {
    auto params = context.getParams();
    std::string archive_path = params.getArchivePath().cStr();
    LOG_DEBUG("[Worker] extractArchiveStream called for '{}'", archive_path);
    ArchiveExtractOptions options;
    for (auto ext_text : params.getEnabledExts()) {
      options.enabled_exts.insert(ext_text.cStr());
    }
    if (params.getMaxEntryBytes() > 0) {
      options.max_entry_bytes = params.getMaxEntryBytes();
    }

    struct PendingMessage {
      bool is_finish = false;
      std::string virtual_path;
      std::string file_name;
      std::string ext;
      uint64_t entry_size = 0;
      int64_t entry_mtime = 0;
      ArchiveItemEvent event = ArchiveItemEvent::kData;
      std::string raw_bytes;
      std::string message;
      soeasy::ipc::ParseStatusCode finish_code = soeasy::ipc::ParseStatusCode::OK;
    };

    struct BridgeState {
      explicit BridgeState(soeasy::ipc::ArchiveStreamReceiver::Client recv) : receiver(kj::mv(recv)) {}

      soeasy::ipc::ArchiveStreamReceiver::Client receiver;
      std::mutex mtx;
      std::condition_variable ack_cv;
      bool has_pending = false;
      PendingMessage pending;
      bool ack_done = false;
      bool ack_keep_going = true;
      bool cancel = false;
      kj::Own<kj::CrossThreadPromiseFulfiller<void>> wake_fulfiller;
      std::thread worker_thread;
    };

    using PumpFn = std::function<kj::Promise<void>(kj::Promise<void>)>;

    struct CancelGuard {
      CancelGuard(std::shared_ptr<BridgeState> s, std::shared_ptr<PumpFn> p) : state(kj::mv(s)), pump(kj::mv(p)) {}
      CancelGuard(const CancelGuard &) = delete;
      CancelGuard &operator=(const CancelGuard &) = delete;
      ~CancelGuard() {
        if (pump) {
          *pump = nullptr;
          pump.reset();
        }
        if (!state) return;
        {
          std::lock_guard<std::mutex> lk(state->mtx);
          state->cancel = true;
          state->ack_keep_going = false;
          state->ack_done = true;
          state->ack_cv.notify_all();
        }
        if (state->worker_thread.joinable()) {
          state->worker_thread.join();
        }
      }
      std::shared_ptr<BridgeState> state;
      std::shared_ptr<PumpFn> pump;
    };

    auto first_paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    auto state = std::make_shared<BridgeState>(params.getReceiver());
    state->wake_fulfiller = kj::mv(first_paf.fulfiller);

    auto send_and_wait_ack = [](const std::shared_ptr<BridgeState> &st, PendingMessage &&msg) -> bool {
      kj::Own<kj::CrossThreadPromiseFulfiller<void>> wake;
      {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (st->cancel || !st->wake_fulfiller) {
          return false;
        }
        st->pending = std::move(msg);
        st->has_pending = true;
        st->ack_done = false;
        wake = kj::mv(st->wake_fulfiller);
      }
      wake->fulfill();

      std::unique_lock<std::mutex> lk(st->mtx);
      st->ack_cv.wait(lk, [&]() { return st->ack_done || st->cancel; });
      return st->ack_keep_going && !st->cancel;
    };

    state->worker_thread = std::thread([state, send_and_wait_ack, archive_path = std::move(archive_path),
                                        options = std::move(options)]() mutable {
      std::string err_msg;
      bool ok = false;
      try {
        ArchiveReader reader;
        ok = reader.Extract(
            archive_path, options,
            [&state, &send_and_wait_ack](const ArchiveItem &item) -> bool {
              PendingMessage msg;
              msg.is_finish = false;
              msg.virtual_path = item.virtual_path;
              msg.file_name = item.file_name;
              msg.ext = item.ext;
              msg.entry_size = item.entry_size;
              msg.entry_mtime = item.entry_mtime;
              msg.event = item.event;
              if (item.raw_data && item.raw_size > 0) {
                msg.raw_bytes.assign(item.raw_data, item.raw_size);
              }
              msg.message = item.message;
              return send_and_wait_ack(state, std::move(msg));
            },
            &err_msg);
      } catch (const std::exception &e) {
        ok = false;
        err_msg = e.what();
      } catch (...) {
        ok = false;
        err_msg = "Unknown exception inside ArchiveReader";
      }

      PendingMessage fin_msg;
      fin_msg.is_finish = true;
      fin_msg.finish_code = ok ? soeasy::ipc::ParseStatusCode::OK : soeasy::ipc::ParseStatusCode::FAILED;
      fin_msg.message = ok ? "Success" : (err_msg.empty() ? "Archive extraction failed" : err_msg);
      send_and_wait_ack(state, std::move(fin_msg));
      common::SystemEnv::TrimHeap();
    });

    auto pump = std::make_shared<PumpFn>();
    std::weak_ptr<PumpFn> weak_pump = pump;
    *pump = [state, weak_pump](kj::Promise<void> ready) -> kj::Promise<void> {
      return ready.then([state, weak_pump]() -> kj::Promise<void> {
        PendingMessage msg;
        kj::Maybe<kj::Promise<void>> next_promise;
        {
          std::lock_guard<std::mutex> lk(state->mtx);
          if (state->cancel) {
            return kj::READY_NOW;
          }
          if (!state->has_pending) {
            auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
            state->wake_fulfiller = kj::mv(paf.fulfiller);
            if (auto locked = weak_pump.lock()) {
              return (*locked)(kj::mv(paf.promise));
            }
            return kj::READY_NOW;
          }
          msg = std::move(state->pending);
          state->has_pending = false;
          if (!msg.is_finish) {
            auto next_paf = kj::newPromiseAndCrossThreadFulfiller<void>();
            state->wake_fulfiller = kj::mv(next_paf.fulfiller);
            next_promise = kj::mv(next_paf.promise);
          }
        }

        if (msg.is_finish) {
          auto req = state->receiver.finishRequest();
          req.setStatusCode(msg.finish_code);
          req.setMessage(msg.message);
          return req.send().then([state](auto &&) {
            std::lock_guard<std::mutex> lk(state->mtx);
            state->ack_done = true;
            state->ack_cv.notify_all();
          });
        }

        auto req = state->receiver.onItemMetaRequest();
        req.setVirtualPath(msg.virtual_path);
        req.setFileName(msg.file_name);
        req.setExt(msg.ext);
        req.setEntrySize(msg.event == ArchiveItemEvent::kData ? msg.raw_bytes.size() : msg.entry_size);
        req.setEntryMtime(msg.entry_mtime);
        switch (msg.event) {
          case ArchiveItemEvent::kEncrypted:
            req.setEvent(soeasy::ipc::ArchiveItemEvent::ENCRYPTED);
            break;
          case ArchiveItemEvent::kSkippedTooLarge:
            req.setEvent(soeasy::ipc::ArchiveItemEvent::SKIPPED_TOO_LARGE);
            break;
          case ArchiveItemEvent::kFailed:
            req.setEvent(soeasy::ipc::ArchiveItemEvent::FAILED);
            break;
          case ArchiveItemEvent::kHeartbeat:
            req.setEvent(soeasy::ipc::ArchiveItemEvent::HEARTBEAT);
            break;
          default:
            req.setEvent(soeasy::ipc::ArchiveItemEvent::DATA);
            break;
        }
        req.setMessage(msg.message);

        const bool is_data = (msg.event == ArchiveItemEvent::kData);
        auto payload = is_data ? std::make_shared<std::string>(std::move(msg.raw_bytes))
                               : std::shared_ptr<std::string>();

        return req.send().then(
            [state, weak_pump, is_data, payload = std::move(payload),
             next_promise = kj::mv(next_promise)](auto resp) mutable -> kj::Promise<void> {
              bool cancelled = false;
              bool keep = resp.getKeepGoing();
              {
                std::lock_guard<std::mutex> lk(state->mtx);
                cancelled = state->cancel;
                if (!is_data || !keep || cancelled) {
                  state->ack_keep_going = keep;
                  state->ack_done = true;
                  state->ack_cv.notify_all();
                }
              }
              if (cancelled) {
                return kj::Promise<void>(kj::READY_NOW);
              }
              if (!is_data || !keep) {
                KJ_IF_MAYBE(np, next_promise) {
                  if (auto locked = weak_pump.lock()) {
                    return (*locked)(kj::mv(*np));
                  }
                }
                return kj::Promise<void>(kj::READY_NOW);
              }

              constexpr size_t kArchiveChunkBytes = 1024ULL * 1024ULL;  // 1 MiB chunk << 64 MiB RPC limit
              kj::Promise<void> chunkChain = kj::READY_NOW;
              if (payload) {
                for (size_t offset = 0; offset < payload->size(); offset += kArchiveChunkBytes) {
                  const size_t len = std::min(kArchiveChunkBytes, payload->size() - offset);
                  chunkChain = chunkChain.then([state, payload, offset, len]() {
                    auto creq = state->receiver.onItemChunkRequest();
                    creq.setChunk(kj::ArrayPtr<const kj::byte>(
                        reinterpret_cast<const kj::byte *>(payload->data() + offset), len));
                    return creq.send().ignoreResult();
                  });
                }
                payload.reset();
              }

              return chunkChain.then([state, weak_pump, next_promise = kj::mv(next_promise)]() mutable {
                auto endReq = state->receiver.onItemEndRequest();
                return endReq.send().then(
                    [state, weak_pump, next_promise = kj::mv(next_promise)](auto endResp) mutable {
                      bool end_cancelled = false;
                      {
                        std::lock_guard<std::mutex> lk(state->mtx);
                        state->ack_keep_going = endResp.getKeepGoing();
                        state->ack_done = true;
                        end_cancelled = state->cancel;
                        state->ack_cv.notify_all();
                      }
                      if (end_cancelled) {
                        return kj::Promise<void>(kj::READY_NOW);
                      }
                      KJ_IF_MAYBE(np, next_promise) {
                        if (auto locked = weak_pump.lock()) {
                          return (*locked)(kj::mv(*np));
                        }
                      }
                      return kj::Promise<void>(kj::READY_NOW);
                    });
              });
            });
      });
    };

    auto start_promise = (*pump)(kj::mv(first_paf.promise));
    auto guard = kj::heap<CancelGuard>(state, std::move(pump));
    return start_promise.attach(kj::mv(guard));
  }

  kj::Promise<void> parseFileStream(ParseFileStreamContext context) override {
    auto params = context.getParams();
    int64_t doc_id = params.getDocId();
    std::string file_path = params.getFilePath().cStr();
    std::string ext = params.getExt().cStr();
    auto receiver = params.getReceiver();

    CheckTestHooks(file_path);

    ParseContext ctx;
    ctx.doc_id = doc_id;
    ctx.file_path = file_path;
    ctx.ext = ext;
    ctx.timeout_ms = params.getTimeoutMs() > 0 ? params.getTimeoutMs() : 60000;

    ExtractedDocument doc;
    auto parse_start = std::chrono::high_resolution_clock::now();
    bool parse_ok = registry_.ParseDocument(ctx, &doc);
    auto parse_end = std::chrono::high_resolution_clock::now();
    double cost_ms = std::chrono::duration<double, std::milli>(parse_end - parse_start).count();

    return StreamDocumentResult(registry_, tasks_since_trim_, file_path, ext, std::move(doc), cost_ms, parse_ok,
                                receiver);
  }

  kj::Promise<void> ping(PingContext context) override {
    auto results = context.getResults();
    results.setRssBytes(0);
    results.setIsBusy(false);
    return kj::READY_NOW;
  }

 private:
  void MaybeTrimHeap(size_t extracted_bytes, double cost_ms) {
    MaybeTrimWorkerHeap(registry_, tasks_since_trim_, extracted_bytes, cost_ms);
  }

  ParserRegistry &registry_;
  std::shared_ptr<size_t> tasks_since_trim_;
};

int RunWorkerLoop(const std::string &rpc_addr, ParserRegistry *custom_registry) {
  std::unique_ptr<ParserRegistry> default_registry;
  if (!custom_registry) {
    default_registry = ParserRegistry::CreateDefaultRegistry();
  }
  ParserRegistry &registry = custom_registry ? *custom_registry : *default_registry;

  try {
    kj::AsyncIoContext ioContext = kj::setupAsyncIo();
    auto addr = ioContext.provider->getNetwork().parseAddress(rpc_addr).wait(ioContext.waitScope);
    auto stream = addr->connect().wait(ioContext.waitScope);
    capnp::TwoPartyServer server(kj::heap<WorkerAgentImpl>(registry));
    server.accept(kj::mv(stream));
    server.drain().wait(ioContext.waitScope);
    LOG_INFO("[RunWorkerLoop] server.drain().wait() completed gracefully.");
    return 0;
  } catch (const kj::Exception &e) {
    LOG_WARN("[RunWorkerLoop] Exited on kj::Exception: {}", e.getDescription().cStr());
    return 0;
  } catch (const std::exception &e) {
    LOG_ERROR("[RunWorkerLoop] Exited on std::exception: {}", e.what());
    return 1;
  } catch (...) {
    LOG_ERROR("[RunWorkerLoop] Exited on unknown exception");
    return 1;
  }
}

}  // namespace worker
}  // namespace soeasy
