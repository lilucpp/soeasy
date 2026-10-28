#ifndef SOEASY_SRC_STORAGE_STORAGE_ENGINE_H_
#define SOEASY_SRC_STORAGE_STORAGE_ENGINE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sqlite3mc_amalgamation.h"

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

class StorageEngine {
 public:
  StorageEngine();
  ~StorageEngine();

  // Disallow copy and assign
  StorageEngine(const StorageEngine&) = delete;
  StorageEngine& operator=(const StorageEngine&) = delete;

  /**
   * @brief Open or create an encrypted SQLite database and initialize schemas.
   *
   * @param db_path Absolute filesystem path to the database file
   * @param key Encryption passphrase / derived key
   * @param dict_dir Path to Jieba dictionary directory
   * @return true on success, false on error
   */
  bool Open(const std::string& db_path, const std::string& key,
            const std::string& dict_dir);

  /**
   * @brief Close database connection.
   */
  void Close();

  /**
   * @brief Check if the database connection is open and active.
   */
  bool is_open() const { return db_ != nullptr; }

  /**
   * @brief Begin an explicit transaction for batch operations.
   * @return true on success or if transaction already started
   */
  bool BeginTransaction();

  /**
   * @brief Commit the current explicit transaction.
   * @return true on success or if no transaction was active
   */
  bool CommitTransaction();

  /**
   * @brief Rollback the current explicit transaction.
   * @return true on success or if no transaction was active
   */
  bool RollbackTransaction();

  /**
   * @brief Check whether an explicit transaction is currently active.
   */
  bool IsInTransaction() const;

  /**
   * @brief Get count of uncommitted indexed documents in current transaction.
   */
  size_t GetUncommittedCount() const { return uncommitted_count_; }

  /**
   * @brief Compact and merge FTS5 index segments.
   */
  bool OptimizeFtsIndex();

  /**
   * @brief Index or re-index a single document.
   *
   * @param doc_id Unique document identifier (e.g. 64-bit path hash)
   * @param file_path Canonical absolute path
   * @param file_name File name with extension
   * @param ext File extension in lowercase (without dot)
   * @param file_size Size of file on disk in bytes
   * @param mtime Modification timestamp
   * @param parse_level Parsing level completed (1..4)
   * @param utf8_content Extracted full plain text
   * @return true on success
   */
  bool IndexDocument(int64_t doc_id, const std::string& file_path,
                     const std::string& file_name, const std::string& ext,
                     uint64_t file_size, int64_t mtime, int parse_level,
                     const std::string& utf8_content);

  /**
   * @brief Delete document from both metadata, content, and FTS5 index.
   */
  bool DeleteDocument(int64_t doc_id);

  /**
   * @brief Clear all indexed files, contents, and FTS tables to prepare for rebuild.
   */
  bool ClearAll();

  /**
   * @brief Perform full-text search against fts_docs.
   *
   * @param query FTS5 query string
   * @param limit Max hits to return
   * @param offset Result pagination offset
   * @return std::vector<SearchResultItem> List of matching documents
   */
  std::vector<SearchResultItem> Search(const std::string& query,
                                       int limit = 50, int offset = 0);

  /**
   * @brief Decompress and fetch original UTF-8 content from doc_contents.
   *
   * @param doc_id Document ID
   * @param out_content Output string to receive decompressed text
   * @return true on success
   */
  bool GetDocumentContent(int64_t doc_id, std::string* out_content);

  /**
   * @brief Retrieve metadata (file_size, mtime, parse_stat) for incremental check.
   *
   * @param doc_id Document ID
   * @param out_file_size Output pointer for file size
   * @param out_mtime Output pointer for mtime
   * @param out_parse_stat Output pointer for parse status
   * @return true if document exists in files table
   */
  bool GetFileMetadata(int64_t doc_id, uint64_t* out_file_size,
                       int64_t* out_mtime, int* out_parse_stat = nullptr);

  /**
   * @brief Retrieve all indexed files metadata for rapid cold-restart.
   */
  std::vector<FileMetaRecord> GetAllFilesMetadata();

  /**
   * @brief Generate highlighted snippet with <mark> tags around keywords.
   */
  static std::string GenerateSnippet(
      const std::string& full_text,
      const std::vector<std::string>& keywords,
      size_t max_chars = 120);

  /**
   * @brief Extract clean keywords from an FTS5 search query string.
   */
  static std::vector<std::string> ExtractKeywordsFromQuery(
      const std::string& query);

  /**
   * @brief Get low-level error message from SQLite.
   */
  std::string GetLastError() const;

 private:
  bool ExecutePragmas(const std::string& key);
  bool InitTables();
  void FinalizeCachedStatements();

  sqlite3* db_ = nullptr;
  std::string last_error_;
  size_t uncommitted_count_ = 0;

  sqlite3_stmt* stmt_files_insert_ = nullptr;
  sqlite3_stmt* stmt_contents_insert_ = nullptr;
  sqlite3_stmt* stmt_fts_del_ = nullptr;
  sqlite3_stmt* stmt_fts_ins_ = nullptr;
  sqlite3_stmt* stmt_meta_select_ = nullptr;
};

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_STORAGE_ENGINE_H_
