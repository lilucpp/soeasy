// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_STORAGE_ISTORAGE_ENGINE_H_
#define SOEASY_SRC_STORAGE_ISTORAGE_ENGINE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace soeasy {
namespace storage {

struct SearchResultItem {
  int64_t doc_id = 0;
  std::string file_name;
  std::string file_path;
  std::string ext;
  uint64_t file_size = 0;
  int64_t mtime = 0;
  double rank = 0.0;
  std::string snippet;
  bool has_body_snippet = false;
};

struct FileMetaRecord {
  int64_t doc_id = 0;
  std::string file_name;
  std::string file_path;
  std::string ext;
  uint64_t file_size = 0;
  int64_t mtime = 0;
  int parse_level = 0;
  int parse_stat = 0;
};

struct StorageFragmentation {
  int page_size = 4096;
  int64_t total_pages = 0;
  int64_t free_pages = 0;
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  double frag_ratio = 0.0;
};

/**
 * @brief RAII scoped lease guard for checked-out read connections.
 *
 * Automatically returns the borrowed sqlite3* handle to ReaderConnectionPool upon destruction.
 */
class ScopedReadConnection {
 public:
  using Releaser = std::function<void(sqlite3 *)>;

  ScopedReadConnection() = default;
  ScopedReadConnection(sqlite3 *db, Releaser releaser)
      : db_(db), releaser_(std::move(releaser)) {}

  ~ScopedReadConnection() {
    if (db_ && releaser_) {
      releaser_(db_);
    }
  }

  // Non-copyable, movable
  ScopedReadConnection(const ScopedReadConnection &) = delete;
  ScopedReadConnection &operator=(const ScopedReadConnection &) = delete;

  ScopedReadConnection(ScopedReadConnection &&other) noexcept
      : db_(other.db_), releaser_(std::move(other.releaser_)) {
    other.db_ = nullptr;
  }

  ScopedReadConnection &operator=(ScopedReadConnection &&other) noexcept {
    if (this != &other) {
      if (db_ && releaser_) {
        releaser_(db_);
      }
      db_ = other.db_;
      releaser_ = std::move(other.releaser_);
      other.db_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] sqlite3 *get() const { return db_; }
  [[nodiscard]] sqlite3 *operator->() const { return db_; }
  [[nodiscard]] explicit operator bool() const { return db_ != nullptr; }

 private:
  sqlite3 *db_ = nullptr;
  Releaser releaser_;
};

/**
 * @brief Abstract contract for the storage and retrieval engine.
 *
 * Decouples write transactions (IndexDocument, Delete, Vacuum) from concurrent
 * reader operations (Search, GetContent, GetMetadata) leveraging SQLite WAL mode.
 */
class IStorageEngine {
 public:
  virtual ~IStorageEngine() = default;

  /**
   * @brief Open or create encrypted SQLite database, initialize schemas, and configure connection pool.
   */
  virtual bool Open(const std::string &db_path, const std::string &key, const std::string &dict_dir) = 0;

  /**
   * @brief Close all connections and drain the reader connection pool.
   */
  virtual void Close() = 0;

  /**
   * @brief Check whether database engine is open and active.
   */
  [[nodiscard]] virtual bool is_open() const = 0;

  /**
   * @brief Begin an explicit write transaction (uses BEGIN IMMEDIATE to claim write lock instantly).
   */
  virtual bool BeginTransaction() = 0;

  /**
   * @brief Commit the current write transaction.
   */
  virtual bool CommitTransaction() = 0;

  /**
   * @brief Rollback the current write transaction.
   */
  virtual bool RollbackTransaction() = 0;

  /**
   * @brief Check whether an explicit transaction is currently active.
   */
  [[nodiscard]] virtual bool IsInTransaction() const = 0;

  /**
   * @brief Get count of uncommitted indexed documents in current transaction.
   */
  [[nodiscard]] virtual size_t GetUncommittedCount() const = 0;

  /**
   * @brief Compact and merge FTS5 index segments.
   */
  virtual bool OptimizeFtsIndex() = 0;

  /**
   * @brief Toggle bulk mode for high-throughput batch operations.
   */
  virtual bool SetBulkMode(bool enable) = 0;

  /**
   * @brief Checkpoint and truncate the WAL file back into the main database.
   */
  virtual bool CheckpointWal() = 0;

  /**
   * @brief Query current database page metrics, freelist count, and fragmentation ratio.
   */
  virtual bool GetFragmentation(StorageFragmentation *out_frag) = 0;

  /**
   * @brief Incrementally reclaim up to N free pages directly to the OS without full rewrite.
   */
  virtual bool IncrementalVacuum(int pages = 500) = 0;

  /**
   * @brief Rebuild and shrink entire database file to reclaim 100% free space.
   */
  virtual bool Vacuum() = 0;

  /**
   * @brief Index or re-index a single document (Write channel).
   */
  virtual bool IndexDocument(int64_t doc_id, const std::string &file_path, const std::string &file_name,
                             const std::string &ext, uint64_t file_size, int64_t mtime, int parse_level,
                             const std::string &utf8_content) = 0;

  /**
   * @brief Delete document from metadata, content, and FTS5 index (Write channel).
   */
  virtual bool DeleteDocument(int64_t doc_id) = 0;

  /**
   * @brief Cascade delete all files under a directory prefix (Write channel).
   */
  virtual int64_t DeleteDirectory(const std::string &dir_path,
                                  std::vector<int64_t> *out_deleted_doc_ids = nullptr) = 0;

  /**
   * @brief Clear all indexed files, contents, and FTS tables to prepare for rebuild (Write channel).
   */
  virtual bool ClearAll() = 0;

  /**
   * @brief Perform full-text search against fts_docs (Read channel via ReaderConnectionPool).
   */
  virtual std::vector<SearchResultItem> Search(const std::string &query, int limit = 50, int offset = 0,
                                               const std::string &path_prefix = "",
                                               int *out_total_count = nullptr) = 0;

  /**
   * @brief Decompress and fetch original UTF-8 content from doc_contents (Read channel).
   */
  virtual bool GetDocumentContent(int64_t doc_id, std::string *out_content) = 0;

  /**
   * @brief Retrieve metadata for incremental check (Read channel).
   */
  virtual bool GetFileMetadata(int64_t doc_id, uint64_t *out_file_size, int64_t *out_mtime,
                               int *out_parse_stat = nullptr) = 0;

  /**
   * @brief Retrieve all indexed files metadata for rapid cold-restart (Read channel).
   */
  virtual std::vector<FileMetaRecord> GetAllFilesMetadata() = 0;

  /**
   * @brief Acquire a dedicated read-only connection handle from the pool (Zero-lock read lease).
   */
  virtual ScopedReadConnection AcquireReadConnection() = 0;

  /**
   * @brief Get low-level error message from SQLite.
   */
  [[nodiscard]] virtual std::string GetLastError() const = 0;
};

/**
 * @brief RAII transaction guard for IStorageEngine.
 *
 * Enforces transaction safety following SQLiteCpp conventions:
 * - Begins transaction upon creation.
 * - If Commit() is not explicitly invoked before destruction, RollbackTransaction() is executed.
 */
class TransactionGuard {
 public:
  explicit TransactionGuard(IStorageEngine &engine)
      : engine_(engine), active_(false), committed_(false) {
    active_ = engine_.BeginTransaction();
  }

  ~TransactionGuard() {
    if (active_ && !committed_) {
      engine_.RollbackTransaction();
    }
  }

  TransactionGuard(const TransactionGuard &) = delete;
  TransactionGuard &operator=(const TransactionGuard &) = delete;
  TransactionGuard(TransactionGuard &&) = delete;
  TransactionGuard &operator=(TransactionGuard &&) = delete;

  [[nodiscard]] bool IsActive() const { return active_; }

  bool Commit() {
    if (active_ && !committed_) {
      committed_ = engine_.CommitTransaction();
      return committed_;
    }
    return false;
  }

  void Rollback() {
    if (active_ && !committed_) {
      engine_.RollbackTransaction();
      active_ = false;
    }
  }

 private:
  IStorageEngine &engine_;
  bool active_{false};
  bool committed_{false};
};

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_ISTORAGE_ENGINE_H_
