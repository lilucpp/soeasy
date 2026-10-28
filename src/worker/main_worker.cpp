#include <cstdlib>
#include <iostream>
#include <string>

#include "common/logger.h"
#include "worker/l2_office_parser.h"
#include "worker/worker_loop.h"

int main(int argc, char* argv[]) {
  int socket_fd = -1;
  std::string shm_name;
  size_t slot_count = 8;
  size_t slot_capacity = 8 * 1024 * 1024;
  std::string log_file = "logs/soeasy_worker.log";
  std::string log_level_str = "info";
  bool enable_console = false;
  bool enable_sync = false;
  bool doctotext_logging = false;
  std::string doctotext_log_path = "";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
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
    }
  }

  auto level = soeasy::common::Logger::ParseLevel(log_level_str);
  soeasy::common::Logger::Init(log_file, 10 * 1024 * 1024, 3, enable_console, level);
  soeasy::common::Logger::SetSyncMode(enable_sync);

  // Configure DocToText logging in this worker process
  soeasy::worker::L2OfficeParser::SetLogging(doctotext_logging, doctotext_log_path);

  if (socket_fd < 0 || shm_name.empty()) {
    std::cerr << "Usage: soeasy-worker --socket-fd=<fd> --shm-name=<name> "
              << "[--slot-count=<N>] [--slot-capacity=<bytes>]\n";
    return 1;
  }

  return soeasy::worker::RunWorkerLoop(socket_fd, shm_name, slot_count,
                                       slot_capacity);
}
