// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_STORAGE_ISTORAGE_ENGINE_H_
#define SOEASY_SRC_STORAGE_ISTORAGE_ENGINE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/resource_profile.h"

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
 * @brief Structured parameters for document indexing to eliminate data clumps.
 */
struct DocumentIndexEntry {
  int64_t doc_id = 0;
  std::string file_path;
  std::string file_name;
  std::string ext;
  uint64_t file_size = 0;
  int64_t mtime = 0;
  int parse_level = 0;
  std::string utf8_content;
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
  ScopedReadConnection(sqlite3 *db, Releaser releaser) : db_(db), releaser_(std::move(releaser)) {}

  ~ScopedReadConnection() {
    if (db_ && releaser_) {
      releaser_(db_);
    }
  }

  // Non-copyable, movable
  ScopedReadConnection(const ScopedReadConnection &) = delete;
  ScopedReadConnection &operator=(const ScopedReadConnection &) = delete;

  ScopedReadConnection(ScopedReadConnection &&other) noexcept : db_(other.db_), releaser_(std::move(other.releaser_)) {
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
  IStorageEngine() = default;
  virtual ~IStorageEngine() = default;

  IStorageEngine(const IStorageEngine &) = delete;
  IStorageEngine &operator=(const IStorageEngine &) = delete;
  IStorageEngine(IStorageEngine &&) = delete;
  IStorageEngine &operator=(IStorageEngine &&) = delete;

  /**
   * @brief Open or create encrypted SQLite database, initialize schemas, and configure connection pool.
   */
  bool Open(const std::string &db_path, const std::string &key, const std::string &dict_dir) {
    return DoOpen(db_path, key, dict_dir);
  }

  /**
   * @brief Close all connections and drain the reader connection pool.
   */
  void Close() { DoClose(); }

  /**
   * @brief Check whether database engine is open and active.
   */
  [[nodiscard]] bool is_open() const { return DoIsOpen(); }

  /**
   * @brief Begin an explicit write transaction (uses BEGIN IMMEDIATE to claim write lock instantly).
   */
  bool BeginTransaction() { return DoBeginTransaction(); }

  /**
   * @brief Commit the current write transaction.
   */
  bool CommitTransaction() { return DoCommitTransaction(); }

  /**
   * @brief Rollback the current write transaction.
   */
  bool RollbackTransaction() { return DoRollbackTransaction(); }

  /**
   * @brief Check whether an explicit transaction is currently active.
   */
  [[nodiscard]] bool IsInTransaction() const { return DoIsInTransaction(); }

  /**
   * @brief Get count of uncommitted indexed documents in current transaction.
   */
  [[nodiscard]] size_t GetUncommittedCount() const { return DoGetUncommittedCount(); }

  /**
   * @brief Compact and merge FTS5 index segments.
   */
  bool OptimizeFtsIndex() { return DoOptimizeFtsIndex(); }

  /**
   * @brief Toggle bulk mode for high-throughput batch operations.
   */
  bool SetBulkMode(bool enable) { return DoSetBulkMode(enable); }

  /**
   * @brief Apply a resource tuning profile at startup or runtime.
   */
  void SetResourceProfile(common::ResourceProfile profile) { DoSetResourceProfile(profile); }

  /**
   * @brief Checkpoint and truncate the WAL file back into the main database.
   */
  bool CheckpointWal() { return DoCheckpointWal(); }

  /**
   * @brief Query current database page metrics, freelist count, and fragmentation ratio.
   */
  bool GetFragmentation(StorageFragmentation *out_frag) { return DoGetFragmentation(out_frag); }

  /**
   * @brief Incrementally reclaim up to N free pages directly to the OS without full rewrite.
   */
  bool IncrementalVacuum(int pages = 500) { return DoIncrementalVacuum(pages); }

  /**
   * @brief Rebuild and shrink entire database file to reclaim 100% free space.
   */
  bool Vacuum() { return DoVacuum(); }

  /**
   * @brief Index or re-index a single document (Write channel) using structured entry.
   */
  bool IndexDocument(const DocumentIndexEntry &entry) { return DoIndexDocument(entry); }

  /**
   * @brief Backward-compatible overload for legacy multi-argument invocation.
   */
  bool IndexDocument(int64_t doc_id, const std::string &file_path, const std::string &file_name, const std::string &ext,
                     uint64_t file_size, int64_t mtime, int parse_level, const std::string &utf8_content) {
    DocumentIndexEntry entry;
    entry.doc_id = doc_id;
    entry.file_path = file_path;
    entry.file_name = file_name;
    entry.ext = ext;
    entry.file_size = file_size;
    entry.mtime = mtime;
    entry.parse_level = parse_level;
    entry.utf8_content = utf8_content;
    return DoIndexDocument(entry);
  }

  /**
   * @brief Delete document from metadata, content, and FTS5 index (Write channel).
   */
  bool DeleteDocument(int64_t doc_id) { return DoDeleteDocument(doc_id); }

  /**
   * @brief Cascade delete all files under a directory prefix (Write channel).
   */
  int64_t DeleteDirectory(const std::string &dir_path, std::vector<int64_t> *out_deleted_doc_ids = nullptr) {
    return DoDeleteDirectory(dir_path, out_deleted_doc_ids);
  }

  /**
   * @brief Clear all indexed files, contents, and FTS tables to prepare for rebuild (Write channel).
   */
  bool ClearAll() { return DoClearAll(); }

  /**
   * @brief Perform full-text search against fts_docs (Read channel via ReaderConnectionPool).
   */
  std::vector<SearchResultItem> Search(const std::string &query, int limit = 50, int offset = 0,
                                       const std::string &path_prefix = "", int *out_total_count = nullptr) {
    return DoSearch(query, limit, offset, path_prefix, out_total_count);
  }

  /**
   * @brief Decompress and fetch original UTF-8 content from doc_contents (Read channel).
   */
  bool GetDocumentContent(int64_t doc_id, std::string *out_content) {
    return DoGetDocumentContent(doc_id, out_content);
  }

  /**
   * @brief Retrieve metadata for incremental check (Read channel).
   */
  bool GetFileMetadata(int64_t doc_id, uint64_t *out_file_size, int64_t *out_mtime, int *out_parse_stat = nullptr) {
    return DoGetFileMetadata(doc_id, out_file_size, out_mtime, out_parse_stat);
  }

  /**
   * @brief Retrieve all indexed files metadata for rapid cold-restart (Read channel).
   */
  std::vector<FileMetaRecord> GetAllFilesMetadata() { return DoGetAllFilesMetadata(); }

  /**
   * @brief Acquire a dedicated read-only connection handle from the pool (Zero-lock read lease).
   */
  ScopedReadConnection AcquireReadConnection() { return DoAcquireReadConnection(); }

  /**
   * @brief Atomically rekey the database to a new encryption key using SQLite3MC.
   *
   * @param new_key New encryption key to apply
   * @return true on success, false on failure (reverts to previous key)
   */
  bool Rekey(const std::string &new_key) { return DoRekey(new_key); }

  /**
   * @brief Get low-level error message from SQLite.
   */
  [[nodiscard]] std::string GetLastError() const { return DoGetLastError(); }

 protected:
  virtual bool DoOpen(const std::string &db_path, const std::string &key, const std::string &dict_dir) = 0;
  virtual void DoClose() = 0;
  [[nodiscard]] virtual bool DoIsOpen() const = 0;

  virtual bool DoBeginTransaction() = 0;
  virtual bool DoCommitTransaction() = 0;
  virtual bool DoRollbackTransaction() = 0;
  [[nodiscard]] virtual bool DoIsInTransaction() const = 0;
  [[nodiscard]] virtual size_t DoGetUncommittedCount() const = 0;

  virtual bool DoOptimizeFtsIndex() = 0;
  virtual bool DoSetBulkMode(bool enable) = 0;
  virtual void DoSetResourceProfile(common::ResourceProfile /*profile*/) {}
  virtual bool DoCheckpointWal() = 0;
  virtual bool DoGetFragmentation(StorageFragmentation *out_frag) = 0;
  virtual bool DoIncrementalVacuum(int pages) = 0;
  virtual bool DoVacuum() = 0;

  virtual bool DoIndexDocument(const DocumentIndexEntry &entry) = 0;
  virtual bool DoDeleteDocument(int64_t doc_id) = 0;
  virtual int64_t DoDeleteDirectory(const std::string &dir_path, std::vector<int64_t> *out_deleted_doc_ids) = 0;
  virtual bool DoClearAll() = 0;

  virtual std::vector<SearchResultItem> DoSearch(const std::string &query, int limit, int offset,
                                                 const std::string &path_prefix, int *out_total_count) = 0;
  virtual bool DoGetDocumentContent(int64_t doc_id, std::string *out_content) = 0;
  virtual bool DoGetFileMetadata(int64_t doc_id, uint64_t *out_file_size, int64_t *out_mtime, int *out_parse_stat) = 0;
  virtual std::vector<FileMetaRecord> DoGetAllFilesMetadata() = 0;
  virtual ScopedReadConnection DoAcquireReadConnection() = 0;
  virtual bool DoRekey(const std::string &new_key) = 0;
  [[nodiscard]] virtual std::string DoGetLastError() const = 0;
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
  explicit TransactionGuard(IStorageEngine &engine) : engine_(engine), active_(false), committed_(false) {
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
