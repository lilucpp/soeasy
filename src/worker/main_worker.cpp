#include <cstdlib>
#include <iostream>
#include <string>

#include "common/logger.h"
#include "worker/worker_loop.h"

int main(int argc, char* argv[]) {
  soeasy::common::Logger::Init("logs/soeasy_worker.log", 10 * 1024 * 1024, 3, true);
  int socket_fd = -1;
  std::string shm_name;
  size_t slot_count = 8;
  size_t slot_capacity = 8 * 1024 * 1024;

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
    }
  }

  if (socket_fd < 0 || shm_name.empty()) {
    std::cerr << "Usage: soeasy-worker --socket-fd=<fd> --shm-name=<name> "
              << "[--slot-count=<N>] [--slot-capacity=<bytes>]\n";
    return 1;
  }

  return soeasy::worker::RunWorkerLoop(socket_fd, shm_name, slot_count,
                                       slot_capacity);
}
