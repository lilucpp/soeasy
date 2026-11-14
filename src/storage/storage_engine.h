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

struct ZSTD_CCtx_s;

namespace soeasy {
namespace storage {

class StorageEngine : public IStorageEngine {
 public:
  StorageEngine();
  ~StorageEngine() override;

  // Disallow copy and assign
  StorageEngine(const StorageEngine &) = delete;
  StorageEngine &operator=(const StorageEngine &) = delete;

 protected:
  bool DoOpen(const std::string &db_path, const std::string &key, const std::string &dict_dir) override;
  void DoClose() override;
  [[nodiscard]] bool DoIsOpen() const override;

  bool DoBeginTransaction() override;
  bool DoCommitTransaction() override;
  bool DoRollbackTransaction() override;
  [[nodiscard]] bool DoIsInTransaction() const override;
  [[nodiscard]] size_t DoGetUncommittedCount() const override { return uncommitted_count_; }

  bool DoOptimizeFtsIndex() override;
  bool DoSetBulkMode(bool enable) override;
  void DoSetResourceProfile(common::ResourceProfile profile) override;
  bool DoCheckpointWal() override;
  bool DoRekey(const std::string &new_key) override;
  bool DoGetFragmentation(StorageFragmentation *out_frag) override;
  bool DoIncrementalVacuum(int pages) override;
  bool DoVacuum() override;

  bool DoIndexDocument(const DocumentIndexEntry &entry) override;
  bool DoDeleteDocument(int64_t doc_id) override;
  int64_t DoDeleteDirectory(const std::string &dir_path, std::vector<int64_t> *out_deleted_doc_ids) override;
  bool DoClearAll() override;

  std::vector<SearchResultItem> DoSearch(const std::string &query, int limit, int offset,
                                         const std::string &path_prefix, int *out_total_count) override;
  bool DoGetDocumentContent(int64_t doc_id, std::string *out_content) override;
  bool DoGetFileMetadata(int64_t doc_id, uint64_t *out_file_size, int64_t *out_mtime, int *out_parse_stat) override;
  std::vector<FileMetaRecord> DoGetAllFilesMetadata() override;
  ScopedReadConnection DoAcquireReadConnection() override;
  [[nodiscard]] std::string DoGetLastError() const override;

 private:
  bool ExecutePragmas(sqlite3 *db, const std::string &key);
  bool InitTables();
  void FinalizeCachedStatements();
  void CloseInternal();

  /**
   * @brief Thread-safe error message helpers.
   *
   * Read-channel calls (Search / GetDocumentContent / GetFileMetadata) execute concurrently on
   * multiple threads, so `last_error_` must never be written without `error_mutex_`.
   */
  void SetLastError(std::string msg);
  [[nodiscard]] std::string LastErrorCopy() const;
  void ClearLastError();

  // Internal unlocked implementations on writer_db_ (writer_mutex_ already held by caller)
  void CloseUnlocked();
  bool CommitTransactionUnlocked();
  bool IsInTransactionUnlocked() const;
  bool CheckpointWalUnlocked();
  void ApplyWriterPragmasUnlocked(sqlite3 *db, int wal_autocheckpoint, int cache_kb, int64_t mmap_bytes);
  void ShrinkIdleMemoryUnlocked();
  bool GetDocumentContentOnDb(sqlite3 *db, int64_t doc_id, std::string *out_content);

  std::unique_ptr<SQLite::Database> writer_cpp_db_;
  sqlite3 *writer_db_ = nullptr;
  mutable std::mutex writer_mutex_;
  ReaderConnectionPool reader_pool_;

  std::string db_path_;
  std::string key_;
  std::string dict_dir_;
  mutable std::mutex error_mutex_;
  std::string last_error_;
  size_t uncommitted_count_ = 0;

  sqlite3_stmt *stmt_files_insert_ = nullptr;
  sqlite3_stmt *stmt_contents_insert_ = nullptr;
  sqlite3_stmt *stmt_fts_del_ = nullptr;
  sqlite3_stmt *stmt_fts_ins_ = nullptr;
  sqlite3_stmt *stmt_meta_select_ = nullptr;
  ::ZSTD_CCtx_s *zstd_cctx_ = nullptr;
  std::vector<uint8_t> compress_buf_;
  common::ResourceProfile resource_profile_ = common::ResourceProfile::kLowMemory;
  bool bulk_mode_active_ = false;
};

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_STORAGE_ENGINE_H_
