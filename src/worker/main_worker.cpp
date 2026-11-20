#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#if defined(__GLIBC__) || defined(__linux__)
#include <malloc.h>
#endif
#endif

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
#if defined(__GLIBC__) || defined(__linux__)
  // Constrain glibc multi-arena memory fragmentation from multi-threaded runtimes (OpenCV / ONNX Runtime)
  mallopt(M_ARENA_MAX, 2);
#endif
  int socket_fd = -1;
  std::string socket_name;
#ifdef _WIN32
  std::string pipe_name;
#endif
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
    } else if (arg.rfind("--socket-name=", 0) == 0) {
      socket_name = arg.substr(14);
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

#ifdef _WIN32
  (void)socket_fd;
  if (pipe_name.empty()) {
    std::cerr << "Usage: soeasy-worker --pipe-name=<name>\n";
    return 1;
  }

  HANDLE h_pipe = CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

  if (h_pipe == INVALID_HANDLE_VALUE) {
    LOG_ERROR("Failed to open pipe '{}', error: {}", pipe_name, GetLastError());
    return 1;
  }

  RedirectStdinToNull("NUL");

  int ret = soeasy::worker::RunWorkerLoop(h_pipe);
  CloseHandle(h_pipe);
  return ret;
#else
  if (socket_fd < 0 && !socket_name.empty()) {
#ifdef SOCK_CLOEXEC
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
#endif
    if (sock >= 0) {
      struct sockaddr_un addr;
      std::memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      addr.sun_path[0] = '\0';
      std::memcpy(addr.sun_path + 1, socket_name.c_str(), socket_name.size());
      socklen_t addr_len = sizeof(sa_family_t) + 1 + socket_name.size();
      if (connect(sock, reinterpret_cast<struct sockaddr *>(&addr), addr_len) == 0) {
        socket_fd = sock;
      } else {
        LOG_ERROR("Failed to connect to abstract socket '{}': {}", socket_name, strerror(errno));
        close(sock);
      }
    }
  }

  if (socket_fd < 0) {
    LOG_ERROR("[Worker Main] socket_fd < 0! socket_name='{}'", socket_name);
    std::cerr << "Usage: soeasy-worker (--socket-name=<name> | --socket-fd=<fd>)\n";
    return 1;
  }

  RedirectStdinToNull("/dev/null");

  LOG_INFO("[Worker Main] Starting RunWorkerLoop on socket_fd={}", socket_fd);
  int ret = soeasy::worker::RunWorkerLoop(socket_fd);
  LOG_INFO("[Worker Main] RunWorkerLoop returned {}", ret);
  close(socket_fd);
  return ret;
#endif
}
