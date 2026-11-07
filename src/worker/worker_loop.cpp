#include "worker/worker_loop.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/io.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "common/encoding_utils.h"
#include "common/logger.h"
#include "common/shm_pool.h"
#include "soeasy_ipc.capnp.h"
#include "worker/l1_text_parser.h"
#include "worker/l2_office_parser.h"
#include "worker/l3_image_parser.h"

namespace soeasy {
namespace worker {

static void ProcessTask(const soeasy::ipc::ParseRequest::Reader &request, common::ShmSlotPool &shm_pool,
                        size_t slot_capacity, L1TextParser &l1_parser, L2OfficeParser &l2_parser,
                        L3ImageParser &l3_parser, capnp::MallocMessageBuilder &response_builder) {
  int64_t doc_id = request.getDocId();
  std::string file_path = request.getFilePath();
  std::string ext = request.getExt();
  uint32_t slot_id = request.getTargetSlot().getSlotId();

  // Check for test hooks: intentional hang to verify timeout watchdog
  if (file_path.find("TEST_TRIGGER_HANG") != std::string::npos) {
    while (true) {
#ifdef _WIN32
      Sleep(100);
#else
      usleep(100000);  // Infinite loop waiting to be killed by WorkerGuard
#endif
    }
  }

  // Check for test hooks: intentional crash to verify crash recovery
  if (file_path.find("TEST_TRIGGER_CRASH") != std::string::npos) {
    volatile int *null_ptr = nullptr;
    // cppcheck-suppress nullPointer
    *null_ptr = 42;  // Trigger SIGSEGV
  }

  // Parse document via L3 Image Parser / L2 Office Parser if supported, otherwise L1 Text Parser
  std::string extracted_utf8;
  bool is_scanned_pdf = false;
  bool parse_ok = false;

  shm_pool.TouchHeartbeat(slot_id);
  auto parse_start = std::chrono::high_resolution_clock::now();
  if (l3_parser.IsSupported(ext)) {
    parse_ok = l3_parser.Parse(file_path, ext, &extracted_utf8);
  } else if (l2_parser.IsSupported(ext)) {
    parse_ok = l2_parser.Parse(file_path, ext, &extracted_utf8, &is_scanned_pdf);
  } else {
    parse_ok = l1_parser.ParseFile(file_path, ext, &extracted_utf8);
  }
  shm_pool.TouchHeartbeat(slot_id);
  auto parse_end = std::chrono::high_resolution_clock::now();
  double cost_ms = std::chrono::duration<double, std::milli>(parse_end - parse_start).count();

  auto result = response_builder.initRoot<soeasy::ipc::ParseResult>();
  result.setDocId(doc_id);
  result.setIsScannedPdf(is_scanned_pdf);

  if (!parse_ok) {
    LOG_ERROR("[Worker] Parse FAILED | file: '{}' | ext: '{}' | cost: {:.2f} ms", file_path, ext, cost_ms);
    result.setStatusCode(soeasy::ipc::ParseStatusCode::FAILED);
    result.setExtractedBytes(0);
    result.setPageCount(0);
    result.setMessage("Failed to parse document");
  } else {
    LOG_INFO("[Worker] Parse SUCCESS | file: '{}' | ext: '{}' | extracted: {} bytes | cost: {:.2f} ms", file_path, ext,
             extracted_utf8.size(), cost_ms);
    size_t text_bytes = extracted_utf8.size();
    if (text_bytes > slot_capacity) {
      // Spill to temporary file to eliminate truncation barrier
      std::error_code ec;
      std::filesystem::path spill_dir = std::filesystem::temp_directory_path(ec) / "soeasy_spill";
      std::filesystem::create_directories(spill_dir, ec);

      auto now_us =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
#ifdef _WIN32
      int my_pid = static_cast<int>(GetCurrentProcessId());
#else
      int my_pid = static_cast<int>(getpid());
#endif
      std::string spill_filename = "soeasy_spill_" + std::to_string(my_pid) + "_" + std::to_string(doc_id) + "_" +
                                   std::to_string(now_us) + ".tmp";
      std::filesystem::path spill_path = spill_dir / spill_filename;

      auto ofs = common::EncodingUtils::OpenOfstream(spill_path.string(), std::ios::binary);
      if (ofs.is_open()) {
        ofs.write(extracted_utf8.data(), extracted_utf8.size());
        ofs.close();
        shm_pool.CommitWrite(slot_id, 0);

        result.setStatusCode(soeasy::ipc::ParseStatusCode::OK);
        result.setExtractedBytes(text_bytes);
        result.setPageCount(1);
        result.setIsScannedPdf(is_scanned_pdf);
        result.setMessage("SPILL:" + spill_path.string());
        LOG_INFO("[Worker] Document exceeded slot capacity ({} > {} bytes). Spilled to '{}'", text_bytes, slot_capacity,
                 spill_path.string());
      } else {
        // Fallback: write up to slot_capacity if spill file creation fails
        uint8_t *slot_data = shm_pool.GetWritableSlotData(slot_id);
        if (slot_data) {
          std::memcpy(slot_data, extracted_utf8.data(), slot_capacity);
          shm_pool.CommitWrite(slot_id, slot_capacity);
        }
        result.setStatusCode(soeasy::ipc::ParseStatusCode::OK);
        result.setExtractedBytes(slot_capacity);
        result.setMessage("Success");
      }
    } else {
      // Write zero-copy into shared memory slot
      uint8_t *slot_data = shm_pool.GetWritableSlotData(slot_id);
      if (slot_data && text_bytes > 0) {
        std::memcpy(slot_data, extracted_utf8.data(), text_bytes);
        shm_pool.CommitWrite(slot_id, text_bytes);
      } else {
        shm_pool.CommitWrite(slot_id, 0);
      }

      result.setStatusCode(soeasy::ipc::ParseStatusCode::OK);
      result.setExtractedBytes(text_bytes);
      result.setPageCount(1);
      result.setIsScannedPdf(is_scanned_pdf);
      result.setMessage("Success");
    }
  }
}

#ifdef _WIN32
int RunWorkerLoop(void *pipe_handle, const std::string &shm_name, size_t slot_count, size_t slot_capacity) {
  common::ShmSlotPool shm_pool;
  if (!shm_pool.Attach(shm_name, slot_count, slot_capacity)) {
    return 1;
  }

  L1TextParser l1_parser;
  L2OfficeParser l2_parser;
  L3ImageParser l3_parser(ocr::OcrMode::kNormal);

  kj::HandleInputStream input_stream(pipe_handle);
  kj::HandleOutputStream output_stream(pipe_handle);

  while (true) {
    DWORD bytes_avail = 0;
    while (true) {
      if (!PeekNamedPipe(pipe_handle, NULL, 0, NULL, &bytes_avail, NULL)) {
        return 0;
      }
      if (bytes_avail > 0) {
        break;
      }
      l3_parser.CheckIdleUnload(60.0);
      Sleep(1000);
    }

    try {
      capnp::InputStreamMessageReader reader(input_stream);
      auto request = reader.getRoot<soeasy::ipc::ParseRequest>();

      capnp::MallocMessageBuilder response_builder;
      ProcessTask(request, shm_pool, slot_capacity, l1_parser, l2_parser, l3_parser, response_builder);

      capnp::writeMessage(output_stream, response_builder);
    } catch (const kj::Exception &e) {
      break;
    } catch (...) {
      break;
    }
  }

  return 0;
}
#else
int RunWorkerLoop(int socket_fd, const std::string &shm_name, size_t slot_count, size_t slot_capacity) {
  pid_t parent_pid = getppid();

  common::ShmSlotPool shm_pool;
  if (!shm_pool.Attach(shm_name, slot_count, slot_capacity)) {
    return 1;
  }

  L1TextParser l1_parser;
  L2OfficeParser l2_parser;
  L3ImageParser l3_parser(ocr::OcrMode::kNormal);

  while (true) {
    struct pollfd pfd;
    pfd.fd = socket_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ret = poll(&pfd, 1, 1000);  // 1-second timeout
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ret == 0) {
      if (getppid() != parent_pid) {
        break;
      }
      // Timeout: periodic check to unload idle heavy models (PP-OCRv6)
      l3_parser.CheckIdleUnload(60.0);
      continue;
    }
    if (!(pfd.revents & POLLIN)) {
      break;
    }

    try {
      capnp::StreamFdMessageReader reader(socket_fd);
      auto request = reader.getRoot<soeasy::ipc::ParseRequest>();

      capnp::MallocMessageBuilder response_builder;
      ProcessTask(request, shm_pool, slot_capacity, l1_parser, l2_parser, l3_parser, response_builder);

      capnp::writeMessageToFd(socket_fd, response_builder);
    } catch (const kj::Exception &e) {
      break;
    } catch (...) {
      break;
    }
  }

  return 0;
}
#endif

}  // namespace worker
}  // namespace soeasy
