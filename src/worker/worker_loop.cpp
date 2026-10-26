#include "worker/worker_loop.h"

#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/io.h>

#include "common/logger.h"
#include "common/shm_pool.h"
#include "soeasy_ipc.capnp.h"
#include "worker/l1_text_parser.h"
#include "worker/l2_office_parser.h"

namespace soeasy {
namespace worker {

int RunWorkerLoop(int socket_fd, const std::string& shm_name,
                  size_t slot_count, size_t slot_capacity) {
  // Guarantee worker is immediately killed by kernel if master process dies
  prctl(PR_SET_PDEATHSIG, SIGKILL);

  common::ShmSlotPool shm_pool;
  if (!shm_pool.Attach(shm_name, slot_count, slot_capacity)) {
    return 1;
  }

  L1TextParser l1_parser;
  L2OfficeParser l2_parser;

  while (true) {
    kj::FdInputStream input_stream(socket_fd);
    kj::BufferedInputStreamWrapper buffered_input(input_stream);

    // Try reading next message; on EOF (disconnect), exit gracefully
    try {
      capnp::StreamFdMessageReader reader(socket_fd);
      auto request = reader.getRoot<soeasy::ipc::ParseRequest>();

      int64_t doc_id = request.getDocId();
      std::string file_path = request.getFilePath();
      std::string ext = request.getExt();
      uint32_t slot_id = request.getTargetSlot().getSlotId();

      // Check for test hooks: intentional hang to verify timeout watchdog
      if (file_path.find("TEST_TRIGGER_HANG") != std::string::npos) {
        while (true) {
          usleep(100000);  // Infinite loop waiting to be killed by WorkerGuard
        }
      }

      // Check for test hooks: intentional crash to verify crash recovery
      if (file_path.find("TEST_TRIGGER_CRASH") != std::string::npos) {
        volatile int* null_ptr = nullptr;
        *null_ptr = 42;  // Trigger SIGSEGV
      }

      // Parse document via L2 Office Parser if supported, otherwise L1 Text Parser
      std::string extracted_utf8;
      bool is_scanned_pdf = false;
      bool parse_ok = false;

      auto parse_start = std::chrono::high_resolution_clock::now();
      if (l2_parser.IsSupported(ext)) {
        parse_ok = l2_parser.Parse(file_path, ext, &extracted_utf8, &is_scanned_pdf);
      } else {
        parse_ok = l1_parser.ParseFile(file_path, ext, &extracted_utf8);
      }
      auto parse_end = std::chrono::high_resolution_clock::now();
      double cost_ms =
          std::chrono::duration<double, std::milli>(parse_end - parse_start).count();

      capnp::MallocMessageBuilder response_builder;
      auto result = response_builder.initRoot<soeasy::ipc::ParseResult>();
      result.setDocId(doc_id);
      result.setIsScannedPdf(is_scanned_pdf);

      if (!parse_ok) {
        LOG_ERROR("[Worker] Parse FAILED | file: '{}' | ext: '{}' | cost: {:.2f} ms",
                  file_path, ext, cost_ms);
        result.setStatusCode(soeasy::ipc::ParseStatusCode::FAILED);
        result.setExtractedBytes(0);
        result.setPageCount(0);
        result.setMessage("Failed to parse document");
      } else {
        LOG_INFO("[Worker] Parse SUCCESS | file: '{}' | ext: '{}' | extracted: {} bytes | cost: {:.2f} ms",
                 file_path, ext, extracted_utf8.size(), cost_ms);
        size_t text_bytes = extracted_utf8.size();
        if (text_bytes > slot_capacity) {
          text_bytes = slot_capacity;
        }

        // Write zero-copy into shared memory slot
        uint8_t* slot_data = shm_pool.GetWritableSlotData(slot_id);
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

      capnp::writeMessageToFd(socket_fd, response_builder);
    } catch (const kj::Exception& e) {
      // Stream closed or error, exit loop
      break;
    } catch (...) {
      break;
    }
  }

  return 0;
}

}  // namespace worker
}  // namespace soeasy
