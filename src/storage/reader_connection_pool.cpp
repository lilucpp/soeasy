// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "storage/reader_connection_pool.h"

#include <chrono>

#include "common/logger.h"
#include "sqlite3mc_amalgamation.h"
#include "storage/fts5_jieba_tokenizer.h"

namespace soeasy {
namespace storage {

ReaderConnectionPool::~ReaderConnectionPool() {
  Close();
}

bool ReaderConnectionPool::Init(const std::string &db_path, const std::string &key, const std::string &dict_dir,
                                size_t pool_size) {
  Close();

  std::lock_guard<std::mutex> lock(mutex_);
  db_path_ = db_path;
  key_ = key;
  dict_dir_ = dict_dir;
  capacity_ = pool_size > 0 ? pool_size : 2;
  is_closed_ = false;
  last_error_.clear();

  all_connections_.reserve(capacity_);
  for (size_t i = 0; i < capacity_; ++i) {
    sqlite3 *conn = CreateReaderConnection();
    if (!conn) {
      LOG_ERROR("[ReaderConnectionPool] Failed to initialize reader connection {}/{}", i + 1, capacity_);
      // Cleanup partially created connections
      for (sqlite3 *c : all_connections_) {
        sqlite3_close_v2(c);
      }
      all_connections_.clear();
      while (!available_connections_.empty()) {
        available_connections_.pop();
      }
      return false;
    }
    all_connections_.push_back(conn);
    available_connections_.push(conn);
  }

  LOG_INFO("[ReaderConnectionPool] Successfully initialized {} WAL read-only connections", capacity_);
  return true;
}

sqlite3 *ReaderConnectionPool::CreateReaderConnection() {
  sqlite3 *db = nullptr;
  int rc = sqlite3_open_v2(db_path_.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
  if (rc != SQLITE_OK) {
    last_error_ = db ? sqlite3_errmsg(db) : "Failed to open read-only database";
    if (db) sqlite3_close_v2(db);
    return nullptr;
  }

  char *err_msg = nullptr;
  // 1. Configure cipher parameters
  if (sqlite3_exec(db, "PRAGMA cipher = 'aes128cbc';", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    last_error_ = err_msg ? err_msg : "PRAGMA cipher failed";
    sqlite3_free(err_msg);
    sqlite3_close_v2(db);
    return nullptr;
  }

  if (sqlite3_exec(db, "PRAGMA kdf_iter = 1;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    last_error_ = err_msg ? err_msg : "PRAGMA kdf_iter failed";
    sqlite3_free(err_msg);
    sqlite3_close_v2(db);
    return nullptr;
  }

  // 2. Set encryption passphrase
  std::string key_sql = "PRAGMA key = '" + key_ + "';";
  if (sqlite3_exec(db, key_sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
    last_error_ = err_msg ? err_msg : "PRAGMA key failed";
    sqlite3_free(err_msg);
    sqlite3_close_v2(db);
    return nullptr;
  }

  // 3. Verify key correctness against schema
  sqlite3_stmt *check_stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master;", -1, &check_stmt, nullptr) != SQLITE_OK) {
    last_error_ = "Reader key verification failed: " + std::string(sqlite3_errmsg(db));
    sqlite3_close_v2(db);
    return nullptr;
  }
  sqlite3_step(check_stmt);
  sqlite3_finalize(check_stmt);

  // 4. Concurrency pragmas
  sqlite3_exec(db, "PRAGMA query_only = ON;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA mmap_size = 268435456;", nullptr, nullptr, nullptr);
  sqlite3_busy_timeout(db, 5000);

  // 5. Register custom FTS5 jieba tokenizer
  if (RegisterFts5JiebaTokenizer(db, dict_dir_) != SQLITE_OK) {
    last_error_ = "Failed to register FTS5 jieba tokenizer on reader connection";
    sqlite3_close_v2(db);
    return nullptr;
  }

  return db;
}

void ReaderConnectionPool::Close() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (is_closed_) {
    return;
  }
  is_closed_ = true;

  // Wake up any threads waiting in Acquire()
  cv_.notify_all();

  // Close all idle connections immediately
  while (!available_connections_.empty()) {
    sqlite3 *conn = available_connections_.front();
    available_connections_.pop();
    if (conn) {
      sqlite3_close_v2(conn);
    }
  }

  // Wait for all in-use connections to be returned (with timeout to prevent hang)
  if (in_use_count_ > 0) {
    close_cv_.wait_for(lock, std::chrono::milliseconds(3000), [this] {
      return in_use_count_ == 0;
    });
  }

  all_connections_.clear();
  capacity_ = 0;
}

ScopedReadConnection ReaderConnectionPool::Acquire(uint32_t wait_timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  bool acquired = cv_.wait_for(lock, std::chrono::milliseconds(wait_timeout_ms), [this] {
    return is_closed_ || !available_connections_.empty();
  });

  if (is_closed_ || !acquired || available_connections_.empty()) {
    return ScopedReadConnection(nullptr, nullptr);
  }

  sqlite3 *conn = available_connections_.front();
  available_connections_.pop();
  ++in_use_count_;

  return ScopedReadConnection(conn, [this](sqlite3 *c) {
    this->Release(c);
  });
}

void ReaderConnectionPool::Release(sqlite3 *conn) {
  if (!conn) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (in_use_count_ > 0) {
    --in_use_count_;
  }

  if (!is_closed_) {
    available_connections_.push(conn);
    cv_.notify_one();
  } else {
    // If pool is already closed, safely finalize the connection on return
    sqlite3_close_v2(conn);
    if (in_use_count_ == 0) {
      close_cv_.notify_all();
    }
  }
}

size_t ReaderConnectionPool::available_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return available_connections_.size();
}

std::string ReaderConnectionPool::GetLastError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

}  // namespace storage
}  // namespace soeasy
