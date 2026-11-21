#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/logger.h"
#include "common/system_env.h"
#include "worker/l2_office_parser.h"
#include "worker/worker_loop.h"

namespace {

void RedirectStdinToNull(const char *null_device) {
  if (!std::freopen(null_device, "r", stdin)) {
    LOG_WARN("Failed to redirect stdin to '{}'", null_device);
  }
}

}  // namespace

int main(int argc, char *argv[]) {
  soeasy::common::SystemEnv::ConfigureAllocator(2);
  std::string rpc_addr;
  std::string log_file = "logs/soeasy_worker.log";
  std::string log_level_str = "info";
  bool enable_console = false;
  bool enable_sync = false;
  bool doctotext_logging = false;
  std::string doctotext_log_path = "";
  bool embedded_image_ocr = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--rpc-addr=", 0) == 0) {
      rpc_addr = arg.substr(11);
    } else if (arg.rfind("--log-level=", 0) == 0) {
      log_level_str = arg.substr(12);
    } else if (arg == "--log-console") {
      enable_console = true;
    } else if (arg == "--no-log-console") {
      enable_console = false;
    } else if (arg == "--log-sync") {
      enable_sync = true;
    } else if (arg == "--no-log-sync") {
      enable_sync = false;
    } else if (arg.rfind("--log-file=", 0) == 0) {
      log_file = arg.substr(11);
    } else if (arg == "--doctotext-logging") {
      doctotext_logging = true;
    } else if (arg == "--no-doctotext-logging") {
      doctotext_logging = false;
    } else if (arg.rfind("--doctotext-log-path=", 0) == 0) {
      doctotext_log_path = arg.substr(21);
    } else if (arg == "--embedded-image-ocr") {
      embedded_image_ocr = true;
    } else if (arg == "--no-embedded-image-ocr") {
      embedded_image_ocr = false;
    }
  }

  auto level = soeasy::common::Logger::ParseLevel(log_level_str);
  soeasy::common::Logger::Init(log_file, 10 * 1024 * 1024, 3, enable_console, level);
  soeasy::common::Logger::SetSyncMode(enable_sync);

  // Optimize worker process file descriptor limits for heavy concurrent parsing
  soeasy::common::SystemEnv::OptimizeProcessFileLimits();

  // Configure DocToText logging and embedded image OCR in this worker process
  soeasy::worker::L2OfficeParser::SetLogging(doctotext_logging, doctotext_log_path);
  soeasy::worker::L2OfficeParser::SetEmbeddedImageOcr(embedded_image_ocr);

  if (rpc_addr.empty()) {
    LOG_ERROR("[Worker Main] Missing --rpc-addr");
    std::cerr << "Usage: soeasy-worker --rpc-addr=<host:port>\n";
    return 1;
  }

#ifdef _WIN32
  RedirectStdinToNull("NUL");
#else
  RedirectStdinToNull("/dev/null");
#endif

  LOG_INFO("[Worker Main] Connecting RunWorkerLoop to {}", rpc_addr);
  int ret = soeasy::worker::RunWorkerLoop(rpc_addr);
  LOG_INFO("[Worker Main] RunWorkerLoop returned {}", ret);
  return ret;
}
