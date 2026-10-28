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

void AsyncWriteQueue::Start(std::shared_ptr<storage::StorageEngine> storage_engine,
                           size_t batch_size,
                           std::chrono::milliseconds batch_timeout,
                           size_t max_queue_size) {
  Stop();

  storage_engine_ = std::move(storage_engine);
  batch_size_ = (batch_size > 0) ? batch_size : 100;
  batch_timeout_ = batch_timeout;
  max_queue_size_ = (max_queue_size > 0) ? max_queue_size : 500;

  stop_requested_ = false;
  flush_requested_ = false;
  is_writing_batch_ = false;
  running_ = true;

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

void AsyncWriteQueue::Enqueue(WriteOp op) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_not_full_.wait(lock, [this]() {
    return stop_requested_ || queue_.size() < max_queue_size_;
  });

  if (stop_requested_) {
    return;
  }

  queue_.push(std::move(op));
  cv_not_empty_.notify_one();
}

void AsyncWriteQueue::Flush() {
  std::unique_lock<std::mutex> lock(mutex_);
  flush_requested_ = true;
  cv_not_empty_.notify_one();

  cv_drained_.wait(lock, [this]() {
    return queue_.empty() && !is_writing_batch_;
  });
  flush_requested_ = false;
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
      cv_not_empty_.wait_for(
          lock, std::chrono::milliseconds(50), [this, &last_commit_time]() {
            if (stop_requested_) return true;
            if (flush_requested_ && !queue_.empty()) return true;
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

    // Execute writes in StorageEngine within an outer transaction
    if (!batch.empty() && storage_engine_) {
      storage_engine_->BeginTransaction();
      for (const auto& op : batch) {
        if (op.type == WriteOpType::kIndex) {
          storage_engine_->IndexDocument(op.doc_id, op.file_path, op.file_name,
                                         op.ext, op.file_size, op.mtime,
                                         op.parse_level, op.content);
        } else if (op.type == WriteOpType::kDelete) {
          storage_engine_->DeleteDocument(op.doc_id);
        }
      }
      storage_engine_->CommitTransaction();
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
    storage_engine_->BeginTransaction();
    for (const auto& op : remaining) {
      if (op.type == WriteOpType::kIndex) {
        storage_engine_->IndexDocument(op.doc_id, op.file_path, op.file_name,
                                       op.ext, op.file_size, op.mtime,
                                       op.parse_level, op.content);
      } else if (op.type == WriteOpType::kDelete) {
        storage_engine_->DeleteDocument(op.doc_id);
      }
    }
    storage_engine_->CommitTransaction();
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    is_writing_batch_ = false;
    cv_drained_.notify_all();
  }
}

}  // namespace service
}  // namespace soeasy
