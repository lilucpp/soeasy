// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_STORAGE_STORAGE_ENGINE_H_
#define SOEASY_SRC_STORAGE_STORAGE_ENGINE_H_

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sqlite3mc_amalgamation.h"
#include "storage/istorage_engine.h"
#include "storage/reader_connection_pool.h"

namespace soeasy {
namespace storage {

class StorageEngine : public IStorageEngine {
 public:
  StorageEngine();
  ~StorageEngine() override;

  // Disallow copy and assign
  StorageEngine(const StorageEngine &) = delete;
  StorageEngine &operator=(const StorageEngine &) = delete;

  /**
   * @brief Open or create encrypted SQLite database, initialize schemas, and configure connection pool.
   *
   * @param db_path Absolute filesystem path to the database file
   * @param key Encryption passphrase / derived key
   * @param dict_dir Path to Jieba dictionary directory
   * @return true on success, false on error
   */
  bool Open(const std::string &db_path, const std::string &key, const std::string &dict_dir) override;

  /**
   * @brief Close write connection and reader connection pool.
   */
  void Close() override;

  /**
   * @brief Check if the database connection is open and active.
   */
  [[nodiscard]] bool is_open() const override { return writer_db_ != nullptr; }

  /**
   * @brief Begin an explicit write transaction using BEGIN IMMEDIATE.
   * @return true on success or if transaction already started
   */
  bool BeginTransaction() override;

  /**
   * @brief Commit the current write transaction.
   * @return true on success or if no transaction was active
   */
  bool CommitTransaction() override;

  /**
   * @brief Rollback the current write transaction.
   * @return true on success or if no transaction was active
   */
  bool RollbackTransaction() override;

  /**
   * @brief Check whether an explicit write transaction is currently active.
   */
  [[nodiscard]] bool IsInTransaction() const override;

  /**
   * @brief Get count of uncommitted indexed documents in current transaction.
   */
  [[nodiscard]] size_t GetUncommittedCount() const override { return uncommitted_count_; }

  /**
   * @brief Compact and merge FTS5 index segments.
   */
  bool OptimizeFtsIndex() override;

  /**
   * @brief Toggle bulk mode for high-throughput batch operations (e.g. initial scan, rebuild).
   */
  bool SetBulkMode(bool enable) override;

  /**
   * @brief Checkpoint and truncate the WAL file back into the main database.
   */
  bool CheckpointWal() override;

  /**
   * @brief Atomically rekey the encrypted database with a new encryption key.
   */
  bool Rekey(const std::string &new_key) override;

  /**
   * @brief Query current database page metrics, freelist count, and fragmentation ratio.
   */
  bool GetFragmentation(StorageFragmentation *out_frag) override;

  /**
   * @brief Incrementally reclaim up to N free pages directly to the OS without full rewrite.
   */
  bool IncrementalVacuum(int pages = 500) override;

  /**
   * @brief Rebuild and shrink entire database file to reclaim 100% free space.
   */
  bool Vacuum() override;

  /**
   * @brief Index or re-index a single document (Write channel).
   */
  bool IndexDocument(const DocumentIndexEntry &entry) override;
  bool IndexDocument(int64_t doc_id, const std::string &file_path, const std::string &file_name, const std::string &ext,
                     uint64_t file_size, int64_t mtime, int parse_level, const std::string &utf8_content) override;

  /**
   * @brief Delete document from metadata, content, and FTS5 index (Write channel).
   */
  bool DeleteDocument(int64_t doc_id) override;

  /**
   * @brief Cascade delete all files under a directory prefix (Write channel).
   */
  int64_t DeleteDirectory(const std::string &dir_path, std::vector<int64_t> *out_deleted_doc_ids = nullptr) override;

  /**
   * @brief Clear all indexed files, contents, and FTS tables to prepare for rebuild (Write channel).
   */
  bool ClearAll() override;

  /**
   * @brief Perform full-text search against fts_docs (Read channel via ReaderConnectionPool).
   */
  std::vector<SearchResultItem> Search(const std::string &query, int limit = 50, int offset = 0,
                                       const std::string &path_prefix = "", int *out_total_count = nullptr) override;

  /**
   * @brief Decompress and fetch original UTF-8 content from doc_contents (Read channel).
   */
  bool GetDocumentContent(int64_t doc_id, std::string *out_content) override;

  /**
   * @brief Retrieve metadata for incremental check (Read channel).
   */
  bool GetFileMetadata(int64_t doc_id, uint64_t *out_file_size, int64_t *out_mtime,
                       int *out_parse_stat = nullptr) override;

  /**
   * @brief Retrieve all indexed files metadata for rapid cold-restart (Read channel).
   */
  std::vector<FileMetaRecord> GetAllFilesMetadata() override;

  /**
   * @brief Acquire a dedicated read-only connection handle from the pool (Zero-lock read lease).
   */
  ScopedReadConnection AcquireReadConnection() override;

  /**
   * @brief Generate highlighted snippet with <mark> tags around keywords.
   */
  static std::string GenerateSnippet(const std::string &full_text, const std::vector<std::string> &keywords,
                                     size_t max_chars = 120);

  /**
   * @brief Extract clean keywords from an FTS5 search query string.
   */
  static std::vector<std::string> ExtractKeywordsFromQuery(const std::string &query);

  /**
   * @brief Get low-level error message from SQLite.
   */
  [[nodiscard]] std::string GetLastError() const override;

 private:
  bool ExecutePragmas(sqlite3 *db, const std::string &key);
  bool InitTables();
  void FinalizeCachedStatements();
  void CloseInternal();

  // Internal unlocked implementations on writer_db_ (writer_mutex_ already held by caller)
  void CloseUnlocked();
  bool CommitTransactionUnlocked();
  bool IsInTransactionUnlocked() const;
  bool CheckpointWalUnlocked();
  bool GetDocumentContentOnDb(sqlite3 *db, int64_t doc_id, std::string *out_content);

  std::unique_ptr<SQLite::Database> writer_cpp_db_;
  sqlite3 *writer_db_ = nullptr;
  mutable std::mutex writer_mutex_;
  ReaderConnectionPool reader_pool_;

  std::string db_path_;
  std::string key_;
  std::string dict_dir_;
  std::string last_error_;
  size_t uncommitted_count_ = 0;

  sqlite3_stmt *stmt_files_insert_ = nullptr;
  sqlite3_stmt *stmt_contents_insert_ = nullptr;
  sqlite3_stmt *stmt_fts_del_ = nullptr;
  sqlite3_stmt *stmt_fts_ins_ = nullptr;
  sqlite3_stmt *stmt_meta_select_ = nullptr;
};

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_STORAGE_ENGINE_H_
