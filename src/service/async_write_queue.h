// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_ASYNC_WRITE_QUEUE_H_
#define SOEASY_SRC_SERVICE_ASYNC_WRITE_QUEUE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "storage/storage_engine.h"

namespace soeasy {
namespace service {

enum class WriteOpType {
  kIndex,
  kDelete
};

struct WriteOp {
  WriteOpType type = WriteOpType::kIndex;
  int64_t doc_id = 0;
  std::string file_path;
  std::string file_name;
  std::string ext;
  uint64_t file_size = 0;
  int64_t mtime = 0;
  int parse_level = 1;
  std::string content;
};

/**
 * @brief Asynchronous bounded batch write queue for decoupled StorageEngine persistence.
 *
 * Implements producer-consumer concurrency: parser / crawl threads enqueue parsed text
 * without blocking on disk I/O, while a dedicated background writer commits transactions
 * in batches (100 items / 10s) with bounded backpressure.
 */
class AsyncWriteQueue {
 public:
  AsyncWriteQueue();
  ~AsyncWriteQueue();

  AsyncWriteQueue(const AsyncWriteQueue&) = delete;
  AsyncWriteQueue& operator=(const AsyncWriteQueue&) = delete;

  /**
   * @brief Start background storage writer thread.
   *
   * @param storage_engine Shared storage engine instance
   * @param batch_size Maximum items to write per transaction batch (default 100)
   * @param batch_timeout Max duration before committing a partial batch (default 10s)
   * @param max_queue_size High watermark for bounded backpressure (default 500)
   */
  void Start(std::shared_ptr<storage::StorageEngine> storage_engine,
             size_t batch_size = 100,
             std::chrono::milliseconds batch_timeout = std::chrono::seconds(10),
             size_t max_queue_size = 500);

  /**
   * @brief Stop the writer thread and drain all pending write operations.
   */
  void Stop();

  /**
   * @brief Enqueue a document index or deletion operation (blocks if queue is full).
   */
  void Enqueue(WriteOp op);

  /**
   * @brief Block until all queued operations are written and committed to disk.
   */
  void Flush();

  /**
   * @brief Return number of pending operations currently in the queue.
   */
  size_t PendingCount() const;

  /**
   * @brief Check whether the background writer is actively running.
   */
  bool is_running() const { return running_; }

 private:
  void WorkerLoop();

  std::shared_ptr<storage::StorageEngine> storage_engine_;
  size_t batch_size_ = 100;
  std::chrono::milliseconds batch_timeout_{10000};
  size_t max_queue_size_ = 500;

  mutable std::mutex mutex_;
  std::condition_variable cv_not_empty_;
  std::condition_variable cv_not_full_;
  std::condition_variable cv_drained_;

  std::queue<WriteOp> queue_;
  bool is_writing_batch_ = false;
  bool flush_requested_ = false;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::thread worker_thread_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_ASYNC_WRITE_QUEUE_H_
