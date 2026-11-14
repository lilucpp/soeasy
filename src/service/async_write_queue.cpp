// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "service/async_write_queue.h"

#include <algorithm>
#include <iostream>

#include "common/logger.h"

namespace soeasy {
namespace service {

AsyncWriteQueue::AsyncWriteQueue() = default;

AsyncWriteQueue::~AsyncWriteQueue() {
  Stop();
}

void AsyncWriteQueue::Start(std::shared_ptr<storage::IStorageEngine> storage_engine, size_t batch_size,
                            std::chrono::milliseconds batch_timeout, size_t max_queue_size) {
  Stop();

  {
    // Publish configuration under the lock so a concurrently running producer can never
    // observe a half-updated queue configuration.
    std::lock_guard<std::mutex> lock(mutex_);
    storage_engine_ = std::move(storage_engine);
    batch_size_ = (batch_size > 0) ? batch_size : 100;
    batch_timeout_ = batch_timeout;
    max_queue_size_ = (max_queue_size > 0) ? max_queue_size : 500;

    stop_requested_ = false;
    flush_waiters_ = 0;
    is_writing_batch_ = false;
    paused_ = false;
    running_ = true;
  }

  worker_thread_ = std::thread(&AsyncWriteQueue::WorkerLoop, this);
}

void AsyncWriteQueue::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
    cv_not_empty_.notify_all();
    cv_not_full_.notify_all();
  }

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

bool AsyncWriteQueue::Enqueue(WriteOp *op) {
  if (!op) {
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  cv_not_full_.wait(lock, [this]() { return stop_requested_ || (!paused_ && queue_.size() < max_queue_size_); });

  if (stop_requested_) {
    return false;
  }

  queue_.push(std::move(*op));
  cv_not_empty_.notify_one();
  return true;
}

void AsyncWriteQueue::Flush() {
  std::unique_lock<std::mutex> lock(mutex_);
  struct FlushWaiterGuard {
    explicit FlushWaiterGuard(size_t *counter) : counter_(counter) { ++(*counter_); }
    ~FlushWaiterGuard() { --(*counter_); }
    FlushWaiterGuard(const FlushWaiterGuard &) = delete;
    FlushWaiterGuard &operator=(const FlushWaiterGuard &) = delete;
    FlushWaiterGuard(FlushWaiterGuard &&) = delete;
    FlushWaiterGuard &operator=(FlushWaiterGuard &&) = delete;
    size_t *counter_;
  } waiter_guard(&flush_waiters_);

  cv_not_empty_.notify_one();
  cv_drained_.wait(lock, [this]() { return (queue_.empty() && !is_writing_batch_) || stop_requested_; });
}

void AsyncWriteQueue::Pause() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!running_) {
    // Writer thread already stopped: nothing can be in flight and no drain is possible.
    return;
  }
  paused_ = true;
  // Wake the worker so it can start draining immediately instead of waiting for the batch timeout.
  cv_not_empty_.notify_one();
  // Wait until everything that was already accepted has been committed to disk.
  cv_drained_.wait(lock, [this]() { return queue_.empty() && !is_writing_batch_; });
}

void AsyncWriteQueue::Resume() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = false;
  }
  cv_not_full_.notify_all();
  cv_not_empty_.notify_one();
}

size_t AsyncWriteQueue::PendingCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void AsyncWriteQueue::WorkerLoop() {
  std::vector<WriteOp> batch;
  batch.reserve(batch_size_);
  auto last_commit_time = std::chrono::steady_clock::now();

  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_not_empty_.wait_for(lock, std::chrono::milliseconds(50), [this, &last_commit_time]() {
        if (stop_requested_) return true;
        if ((flush_waiters_ > 0 || paused_) && !queue_.empty()) return true;
        if (queue_.empty()) return false;
        if (queue_.size() >= batch_size_) return true;
        auto now = std::chrono::steady_clock::now();
        return (now - last_commit_time) >= batch_timeout_;
      });

      if (queue_.empty()) {
        if (stop_requested_) {
          break;
        }
        continue;
      }

      while (!queue_.empty() && batch.size() < batch_size_) {
        batch.push_back(std::move(queue_.front()));
        queue_.pop();
      }

      is_writing_batch_ = true;
      cv_not_full_.notify_all();
    }

    // Execute writes in StorageEngine within an outer transaction guarded by RAII
    if (!batch.empty() && storage_engine_) {
      ExecuteBatchTransaction(batch);
      last_commit_time = std::chrono::steady_clock::now();
      batch.clear();
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      is_writing_batch_ = false;
      if (queue_.empty()) {
        cv_drained_.notify_all();
      }
    }
  }

  // Drain any remaining items upon shutdown
  std::vector<WriteOp> remaining;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      remaining.push_back(std::move(queue_.front()));
      queue_.pop();
    }
    is_writing_batch_ = !remaining.empty();
  }

  if (!remaining.empty() && storage_engine_) {
    ExecuteBatchTransaction(remaining);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    is_writing_batch_ = false;
    cv_drained_.notify_all();
  }
}

void AsyncWriteQueue::ExecuteBatchTransaction(const std::vector<WriteOp> &batch) {
  if (batch.empty() || !storage_engine_) {
    return;
  }

  auto execute_op = [this](const WriteOp &op) {
    if (op.type == WriteOpType::kIndex) {
      return storage_engine_->IndexDocument(op.doc_id, op.file_path, op.file_name, op.ext, op.file_size, op.mtime,
                                            op.parse_level, op.content);
    }
    if (op.type == WriteOpType::kDelete) {
      return storage_engine_->DeleteDocument(op.doc_id);
    }
    return false;
  };

  storage::TransactionGuard tx(*storage_engine_);
  bool all_success = true;
  for (const auto &op : batch) {
    if (!execute_op(op)) {
      all_success = false;
    }
  }

  if (all_success) {
    tx.Commit();
  } else {
    LOG_WARN("[AsyncWriteQueue] Batch transaction failed (size={}), rolling back and replaying per-op", batch.size());
    tx.Rollback();
    for (const auto &op : batch) {
      storage::TransactionGuard single_tx(*storage_engine_);
      if (execute_op(op)) {
        single_tx.Commit();
      } else {
        single_tx.Rollback();
        LOG_ERROR("[AsyncWriteQueue] Isolated write operation failed for doc_id={} ('{}')", op.doc_id, op.file_path);
      }
    }
  }
}

}  // namespace service
}  // namespace soeasy
