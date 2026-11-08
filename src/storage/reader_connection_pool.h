// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_STORAGE_READER_CONNECTION_POOL_H_
#define SOEASY_SRC_STORAGE_READER_CONNECTION_POOL_H_

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "storage/istorage_engine.h"

struct sqlite3;

namespace soeasy {
namespace storage {

/**
 * @brief Thread-safe connection pool for SQLite read-only handles (WAL mode).
 *
 * Pre-populates and manages a pool of decrypted, FTS5-registered, query-only SQLite connections.
 * Provides zero-lock reader concurrency alongside single dedicated writer.
 */
class ReaderConnectionPool {
 public:
  ReaderConnectionPool() = default;
  ~ReaderConnectionPool();

  ReaderConnectionPool(const ReaderConnectionPool &) = delete;
  ReaderConnectionPool &operator=(const ReaderConnectionPool &) = delete;
  ReaderConnectionPool(ReaderConnectionPool &&) = delete;
  ReaderConnectionPool &operator=(ReaderConnectionPool &&) = delete;

  /**
   * @brief Initialize reader pool with pre-configured read-only connections.
   *
   * @param db_path Filesystem path to database
   * @param key Passphrase for SQLite3MultipleCiphers AES-128
   * @param dict_dir Path to Jieba dictionary for FTS5 tokenizer
   * @param pool_size Number of concurrent read handles (default 4)
   * @return true on success, false if opening or decryption fails
   */
  bool Init(const std::string &db_path, const std::string &key, const std::string &dict_dir, size_t pool_size = 4);

  /**
   * @brief Close and finalize all pooled reader connections.
   */
  void Close();

  /**
   * @brief Borrow an available read connection from the pool wrapped in RAII ScopedReadConnection.
   * Blocks if all connections are currently checked out.
   */
  ScopedReadConnection Acquire(uint32_t wait_timeout_ms = 5000);

  /**
   * @brief Return a borrowed read connection handle back to the pool.
   */
  void Release(sqlite3 *conn);

  /**
   * @brief Total number of reader connections managed by this pool.
   */
  [[nodiscard]] size_t capacity() const { return capacity_; }

  /**
   * @brief Current count of idle/available reader connections.
   */
  [[nodiscard]] size_t available_count() const;

  /**
   * @brief Get last initialization or runtime error.
   */
  [[nodiscard]] std::string GetLastError() const;

 private:
  sqlite3 *CreateReaderConnection();

  std::string db_path_;
  std::string key_;
  std::string dict_dir_;
  size_t capacity_ = 0;
  bool is_closed_ = false;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable close_cv_;
  std::queue<sqlite3 *> available_connections_;
  std::vector<sqlite3 *> all_connections_;
  size_t in_use_count_ = 0;
  std::string last_error_;
};

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_READER_CONNECTION_POOL_H_
