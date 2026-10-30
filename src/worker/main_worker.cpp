#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logger.h"
#include "worker/l2_office_parser.h"
#include "worker/worker_loop.h"

int main(int argc, char* argv[]) {
  int socket_fd = -1;
#ifdef _WIN32
  std::string pipe_name;
#endif
  std::string shm_name;
  size_t slot_count = 8;
  size_t slot_capacity = 8 * 1024 * 1024;
  std::string log_file = "logs/soeasy_worker.log";
  std::string log_level_str = "info";
  bool enable_console = false;
  bool enable_sync = false;
  bool doctotext_logging = false;
  std::string doctotext_log_path = "";
  bool embedded_image_ocr = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
#ifdef _WIN32
    if (arg.rfind("--pipe-name=", 0) == 0) {
      pipe_name = arg.substr(12);
    } else
#endif
    if (arg.rfind("--socket-fd=", 0) == 0) {
      socket_fd = std::stoi(arg.substr(12));
    } else if (arg.rfind("--shm-name=", 0) == 0) {
      shm_name = arg.substr(11);
    } else if (arg.rfind("--slot-count=", 0) == 0) {
      slot_count = std::stoull(arg.substr(13));
    } else if (arg.rfind("--slot-capacity=", 0) == 0) {
      slot_capacity = std::stoull(arg.substr(16));
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

  // Configure DocToText logging and embedded image OCR in this worker process
  soeasy::worker::L2OfficeParser::SetLogging(doctotext_logging, doctotext_log_path);
  soeasy::worker::L2OfficeParser::SetEmbeddedImageOcr(embedded_image_ocr);

#ifdef _WIN32
  if (pipe_name.empty() || shm_name.empty()) {
    std::cerr << "Usage: soeasy-worker --pipe-name=<name> --shm-name=<name> "
              << "[--slot-count=<N>] [--slot-capacity=<bytes>]\n";
    return 1;
  }

  HANDLE h_pipe = CreateFileA(
      pipe_name.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      NULL,
      OPEN_EXISTING,
      0,
      NULL);

  if (h_pipe == INVALID_HANDLE_VALUE) {
    LOG_ERROR("Failed to open pipe '{}', error: {}", pipe_name, GetLastError());
    return 1;
  }

  int ret = soeasy::worker::RunWorkerLoop(h_pipe, shm_name, slot_count, slot_capacity);
  CloseHandle(h_pipe);
  return ret;
#else
  if (socket_fd < 0 || shm_name.empty()) {
    std::cerr << "Usage: soeasy-worker --socket-fd=<fd> --shm-name=<name> "
              << "[--slot-count=<N>] [--slot-capacity=<bytes>]\n";
    return 1;
  }

  return soeasy::worker::RunWorkerLoop(socket_fd, shm_name, slot_count,
                                       slot_capacity);
#endif
}
