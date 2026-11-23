#include "worker/worker_loop.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/prctl.h>
#include <unistd.h>
#endif

#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>

#include <chrono>
#include <csignal>
#include <memory>
#include <string>

#include "common/logger.h"
#include "common/system_env.h"
#include "soeasy_ipc.capnp.h"
#include "worker/parser_registry.h"

namespace soeasy {
namespace worker {

class WorkerAgentImpl final : public soeasy::ipc::WorkerAgent::Server {
 public:
  explicit WorkerAgentImpl(ParserRegistry &registry) : registry_(registry) {}

  kj::Promise<void> parseFile(ParseFileContext context) override {
    auto params = context.getParams();
    int64_t doc_id = params.getDocId();
    std::string file_path = params.getFilePath().cStr();
    std::string ext = params.getExt().cStr();

    CheckTestHooks(file_path);

    ParseContext ctx;
    ctx.doc_id = doc_id;
    ctx.file_path = file_path;
    ctx.ext = ext;

    ExtractedDocument doc;
    auto parse_start = std::chrono::high_resolution_clock::now();
    bool parse_ok = registry_.ParseDocument(ctx, &doc);
    auto parse_end = std::chrono::high_resolution_clock::now();
    double cost_ms = std::chrono::duration<double, std::milli>(parse_end - parse_start).count();

    auto result = context.getResults().initResult();
    result.setDocId(doc_id);
    result.setIsScannedPdf(doc.is_scanned_pdf);

    if (!parse_ok) {
      const auto failure = BuildFailureStatusAndLog(file_path, ext, doc, cost_ms, /*is_stream=*/false);
      result.setStatusCode(failure.first);
      result.setExtractedBytes(0);
      result.setPageCount(0);
      result.setMessage(failure.second);
    } else {
      LOG_INFO("[Worker] Parse SUCCESS | file: '{}' | ext: '{}' | extracted: {} bytes | cost: {:.2f} ms", file_path, ext,
               doc.utf8_text.size(), cost_ms);
      result.setStatusCode(soeasy::ipc::ParseStatusCode::OK);
      result.setExtractedBytes(doc.utf8_text.size());
      result.setPageCount(doc.page_count > 0 ? doc.page_count : 1);
      result.setIsScannedPdf(doc.is_scanned_pdf);
      result.setMessage("Success");
      result.setExtractedText(doc.utf8_text);
    }
    const size_t extracted_bytes = doc.utf8_text.size();
    std::string().swap(doc.utf8_text);
    MaybeTrimHeap(extracted_bytes, cost_ms);
    return kj::READY_NOW;
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

    ExtractedDocument doc;
    auto parse_start = std::chrono::high_resolution_clock::now();
    bool parse_ok = registry_.ParseDocument(ctx, &doc);
    auto parse_end = std::chrono::high_resolution_clock::now();
    double cost_ms = std::chrono::duration<double, std::milli>(parse_end - parse_start).count();

    if (!parse_ok) {
      const auto failure = BuildFailureStatusAndLog(file_path, ext, doc, cost_ms, /*is_stream=*/true);
      std::string().swap(doc.utf8_text);
      MaybeTrimHeap(0, cost_ms);
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

    return streamPromise.then([this, receiver, is_scanned, text_bytes, cost_ms]() mutable {
      MaybeTrimHeap(text_bytes, cost_ms);
      auto finishReq = receiver.finishRequest();
      finishReq.setTotalBytes(text_bytes);
      finishReq.setIsScannedPdf(is_scanned);
      finishReq.setStatusCode(soeasy::ipc::ParseStatusCode::OK);
      finishReq.setErrorMsg("Success");
      return finishReq.send().then([](auto &&) {});
    });
  }

  kj::Promise<void> ping(PingContext context) override {
    auto results = context.getResults();
    results.setRssBytes(0);
    results.setIsBusy(false);
    return kj::READY_NOW;
  }

 private:
  static std::pair<soeasy::ipc::ParseStatusCode, std::string> BuildFailureStatusAndLog(const std::string &file_path,
                                                                                       const std::string &ext,
                                                                                       const ExtractedDocument &doc,
                                                                                       double cost_ms,
                                                                                       bool is_stream) {
    const bool is_encrypted = (doc.outcome == ParseOutcome::kEncrypted);
    const auto status_code =
        is_encrypted ? soeasy::ipc::ParseStatusCode::ENCRYPTED : soeasy::ipc::ParseStatusCode::FAILED;
    const std::string err_msg =
        doc.error_message.empty()
            ? (is_encrypted ? "Document is encrypted and requires a password" : "Failed to parse document")
            : doc.error_message;
    const char *mode_suffix = is_stream ? " (stream)" : "";
    if (is_encrypted) {
      LOG_WARN("[Worker] Parse ENCRYPTED{} | file: '{}' | ext: '{}' | cost: {:.2f} ms | {}", mode_suffix, file_path,
               ext, cost_ms, err_msg);
    } else {
      LOG_ERROR("[Worker] Parse FAILED{} | file: '{}' | ext: '{}' | cost: {:.2f} ms | {}", mode_suffix, file_path, ext,
                cost_ms, err_msg);
    }
    return {status_code, err_msg};
  }

  static void CheckTestHooks(const std::string &file_path) {
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

  void MaybeTrimHeap(size_t extracted_bytes, double cost_ms) {
    ++tasks_since_trim_;
    if (extracted_bytes >= 64 * 1024 || cost_ms >= 15.0 || tasks_since_trim_ >= 32) {
      tasks_since_trim_ = 0;
      registry_.CheckIdleUnload(60.0);
      common::SystemEnv::TrimHeap();
    }
  }

  ParserRegistry &registry_;
  size_t tasks_since_trim_ = 0;
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
