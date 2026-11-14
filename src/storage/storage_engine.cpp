// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "storage/storage_engine.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "storage/fts5_jieba_tokenizer.h"
#include "utf8.h"
#include "zstd.h"

namespace soeasy {
namespace storage {

namespace {

/**
 * @brief Zero-overhead RAII guard for sqlite3_stmt* lifecycle management.
 * Guarantees sqlite3_finalize is called on all exit paths (normal, error, or exception).
 */
class SqliteStmtScope {
 public:
  explicit SqliteStmtScope(sqlite3_stmt *stmt = nullptr) : stmt_(stmt) {}
  ~SqliteStmtScope() { Reset(); }

  SqliteStmtScope(const SqliteStmtScope &) = delete;
  SqliteStmtScope &operator=(const SqliteStmtScope &) = delete;

  SqliteStmtScope(SqliteStmtScope &&other) noexcept : stmt_(other.stmt_) { other.stmt_ = nullptr; }
  SqliteStmtScope &operator=(SqliteStmtScope &&other) noexcept {
    if (this != &other) {
      Reset();
      stmt_ = other.stmt_;
      other.stmt_ = nullptr;
    }
    return *this;
  }

  void Reset(sqlite3_stmt *stmt = nullptr) {
    if (stmt_) {
      sqlite3_finalize(stmt_);
    }
    stmt_ = stmt;
  }

  sqlite3_stmt *get() const { return stmt_; }
  sqlite3_stmt **address() {
    Reset();
    return &stmt_;
  }
  sqlite3_stmt *release() {
    sqlite3_stmt *tmp = stmt_;
    stmt_ = nullptr;
    return tmp;
  }
  operator sqlite3_stmt *() const { return stmt_; }

 private:
  sqlite3_stmt *stmt_{nullptr};
};

inline std::string SafeColumnText(sqlite3_stmt *stmt, int col) {
  const auto *text = sqlite3_column_text(stmt, col);
  return text ? reinterpret_cast<const char *>(text) : "";
}

/**
 * @brief Sanitize raw query into exact phrase tokens to recover from FTS5 syntax errors.
 * Wraps tokens in double-quotes and strips unescaped quotes so FTS5 treats them as literals.
 */
std::string BuildSanitizedFtsQuery(const std::string &raw_query) {
  std::istringstream iss(raw_query);
  std::string token;
  std::string sanitized;
  while (iss >> token) {
    std::string clean;
    for (char c : token) {
      if (c != '"') {
        clean.push_back(c);
      }
    }
    if (clean.empty()) continue;
    if (!sanitized.empty()) {
      sanitized.push_back(' ');
    }
    sanitized.push_back('"');
    sanitized.append(clean);
    sanitized.push_back('"');
  }
  return sanitized;
}

std::string EscapeLikePattern(const std::string &input) {
  std::string out;
  out.reserve(input.size() * 2);
  for (char c : input) {
    if (c == '%' || c == '_' || c == '^') {
      out.push_back('^');
    }
    out.push_back(c);
  }
  return out;
}
}  // namespace

StorageEngine::StorageEngine() = default;

StorageEngine::~StorageEngine() {
  CloseInternal();
}

void StorageEngine::FinalizeCachedStatements() {
  if (stmt_files_insert_) {
    sqlite3_finalize(stmt_files_insert_);
    stmt_files_insert_ = nullptr;
  }
  if (stmt_contents_insert_) {
    sqlite3_finalize(stmt_contents_insert_);
    stmt_contents_insert_ = nullptr;
  }
  if (stmt_fts_del_) {
    sqlite3_finalize(stmt_fts_del_);
    stmt_fts_del_ = nullptr;
  }
  if (stmt_fts_ins_) {
    sqlite3_finalize(stmt_fts_ins_);
    stmt_fts_ins_ = nullptr;
  }
  if (stmt_meta_select_) {
    sqlite3_finalize(stmt_meta_select_);
    stmt_meta_select_ = nullptr;
  }
}

void StorageEngine::CloseInternal() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  CloseUnlocked();
}

void StorageEngine::DoClose() {
  CloseInternal();
}

bool StorageEngine::DoIsOpen() const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return writer_db_ != nullptr;
}

void StorageEngine::CloseUnlocked() {
  FinalizeCachedStatements();
  reader_pool_.Close();
  writer_cpp_db_.reset();
  writer_db_ = nullptr;
  uncommitted_count_ = 0;
}

bool StorageEngine::DoBeginTransaction() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    SetLastError("Database not open");
    return false;
  }
  if (IsInTransactionUnlocked()) {
    return true;  // Transaction already active
  }
  char *err = nullptr;
  // Use BEGIN IMMEDIATE to eliminate lock upgrade deadlocks in WAL mode
  int rc = sqlite3_exec(writer_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    SetLastError(err ? err : "BEGIN IMMEDIATE failed");
    sqlite3_free(err);
    return false;
  }
  uncommitted_count_ = 0;
  return true;
}

bool StorageEngine::DoCommitTransaction() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return CommitTransactionUnlocked();
}

bool StorageEngine::CommitTransactionUnlocked() {
  if (!writer_db_) {
    SetLastError("Database not open");
    return false;
  }
  if (!IsInTransactionUnlocked()) {
    return true;  // Nothing to commit
  }
  char *err = nullptr;
  int rc = sqlite3_exec(writer_db_, "COMMIT;", nullptr, nullptr, &err);
  uncommitted_count_ = 0;
  if (rc != SQLITE_OK) {
    SetLastError(err ? err : "COMMIT failed");
    sqlite3_free(err);
    return false;
  }
  return true;
}

bool StorageEngine::DoRollbackTransaction() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    SetLastError("Database not open");
    return false;
  }
  if (!IsInTransactionUnlocked()) {
    return true;
  }
  char *err = nullptr;
  int rc = sqlite3_exec(writer_db_, "ROLLBACK;", nullptr, nullptr, &err);
  uncommitted_count_ = 0;
  if (rc != SQLITE_OK) {
    SetLastError(err ? err : "ROLLBACK failed");
    sqlite3_free(err);
    return false;
  }
  return true;
}

bool StorageEngine::DoIsInTransaction() const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return IsInTransactionUnlocked();
}

bool StorageEngine::IsInTransactionUnlocked() const {
  if (!writer_db_) {
    return false;
  }
  return sqlite3_get_autocommit(writer_db_) == 0;
}

bool StorageEngine::DoOptimizeFtsIndex() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    SetLastError("Database not open");
    return false;
  }
  char *err = nullptr;
  int rc = sqlite3_exec(writer_db_, "INSERT INTO fts_docs(fts_docs) VALUES('optimize');", nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    SetLastError(err ? err : "OptimizeFtsIndex failed");
    sqlite3_free(err);
    return false;
  }
  return true;
}

bool StorageEngine::DoCheckpointWal() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return CheckpointWalUnlocked();
}

bool StorageEngine::CheckpointWalUnlocked() {
  if (!writer_db_) {
    SetLastError("Database not open");
    return false;
  }
  char *err = nullptr;
  int rc = sqlite3_exec(writer_db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    SetLastError(err ? err : "PRAGMA wal_checkpoint failed");
    sqlite3_free(err);
    return false;
  }
  return true;
}

bool StorageEngine::DoRekey(const std::string &new_key) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    SetLastError("Database not open");
    return false;
  }
  if (IsInTransactionUnlocked()) {
    SetLastError("Cannot rekey while in active transaction");
    return false;
  }
  if (!CheckpointWalUnlocked()) {
    SetLastError("Checkpoint failed before rekey: " + LastErrorCopy());
    return false;
  }

  // Drain and close read connection pool before rekey
  reader_pool_.Close();

  LOG_INFO("[StorageEngine] Executing atomic database rekey with SQLite3MC...");
  int rc = sqlite3_rekey(writer_db_, new_key.c_str(), static_cast<int>(new_key.size()));
  if (rc != SQLITE_OK) {
    SetLastError(sqlite3_errmsg(writer_db_));
    LOG_ERROR("[StorageEngine] sqlite3_rekey failed: {}", LastErrorCopy());
    // Fallback: restore reader pool with old key
    reader_pool_.Init(db_path_, key_, dict_dir_, 4);
    return false;
  }

  // Update current key and re-initialize reader connection pool with new key
  key_ = new_key;
  if (!reader_pool_.Init(db_path_, new_key, dict_dir_, 4)) {
    LOG_WARN("[StorageEngine] ReaderConnectionPool failed to re-init with new key: {}", reader_pool_.GetLastError());
  }

  LOG_INFO("[StorageEngine] Database successfully rekeyed to new key.");
  return true;
}

bool StorageEngine::DoGetFragmentation(StorageFragmentation *out_frag) {
  if (!out_frag) {
    return false;
  }

  // Prefer reading metrics from read pool without locking writer_mutex_
  ScopedReadConnection read_conn = AcquireReadConnection();
  sqlite3 *target_db = read_conn.get();
  std::unique_lock<std::mutex> fallback_lock;
  if (!target_db) {
    fallback_lock = std::unique_lock<std::mutex>(writer_mutex_);
    target_db = writer_db_;
  }
  if (!target_db) {
    SetLastError("Database not open");
    return false;
  }

  int page_size = 4096;
  int64_t page_count = 0;
  int64_t free_count = 0;

  if (target_db == writer_db_ && writer_cpp_db_) {
    try {
      SQLite::Statement stmt_ps(*writer_cpp_db_, "PRAGMA page_size;");
      if (stmt_ps.executeStep()) page_size = stmt_ps.getColumn(0).getInt();

      SQLite::Statement stmt_pc(*writer_cpp_db_, "PRAGMA page_count;");
      if (stmt_pc.executeStep()) page_count = stmt_pc.getColumn(0).getInt64();

      SQLite::Statement stmt_fc(*writer_cpp_db_, "PRAGMA freelist_count;");
      if (stmt_fc.executeStep()) free_count = stmt_fc.getColumn(0).getInt64();
    } catch (const std::exception &e) {
      SetLastError(e.what());
      return false;
    }
  } else {
    SqliteStmtScope stmt;
    if (sqlite3_prepare_v2(target_db, "PRAGMA page_size;", -1, stmt.address(), nullptr) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        page_size = sqlite3_column_int(stmt, 0);
      }
    }
    if (sqlite3_prepare_v2(target_db, "PRAGMA page_count;", -1, stmt.address(), nullptr) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        page_count = sqlite3_column_int64(stmt, 0);
      }
    }
    if (sqlite3_prepare_v2(target_db, "PRAGMA freelist_count;", -1, stmt.address(), nullptr) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        free_count = sqlite3_column_int64(stmt, 0);
      }
    }
  }

  out_frag->page_size = page_size;
  out_frag->total_pages = page_count;
  out_frag->free_pages = free_count;
  out_frag->total_bytes = static_cast<uint64_t>(page_count * page_size);
  out_frag->free_bytes = static_cast<uint64_t>(free_count * page_size);
  out_frag->frag_ratio = (page_count > 0) ? (static_cast<double>(free_count) / static_cast<double>(page_count)) : 0.0;
  return true;
}

bool StorageEngine::DoIncrementalVacuum(int pages) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    SetLastError("Database not open");
    return false;
  }
  char *err = nullptr;
  std::string sql;
  if (pages > 0) {
    sql = "PRAGMA incremental_vacuum(" + std::to_string(pages) + ");";
  } else {
    sql = "PRAGMA incremental_vacuum;";
  }
  int rc = sqlite3_exec(writer_db_, sql.c_str(), nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    SetLastError(err ? err : "PRAGMA incremental_vacuum failed");
    sqlite3_free(err);
    return false;
  }
  return true;
}

bool StorageEngine::DoVacuum() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    SetLastError("Database not open");
    return false;
  }
  if (IsInTransactionUnlocked()) {
    CommitTransactionUnlocked();
  }
  char *err = nullptr;
  int rc = sqlite3_exec(writer_db_, "VACUUM;", nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    SetLastError(err ? err : "VACUUM failed");
    sqlite3_free(err);
    return false;
  }
  CheckpointWalUnlocked();
  return true;
}

bool StorageEngine::DoSetBulkMode(bool enable) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    SetLastError("Database not open");
    return false;
  }
  if (enable) {
    // Elevate autocheckpoint interval to prevent checkpoint stalls during bulk indexing
    sqlite3_exec(writer_db_, "PRAGMA wal_autocheckpoint = 10000;", nullptr, nullptr, nullptr);
    sqlite3_exec(writer_db_, "PRAGMA cache_size = -64000;", nullptr, nullptr, nullptr);  // 64MB cache
  } else {
    // Restore default autocheckpoint threshold and truncate WAL
    sqlite3_exec(writer_db_, "PRAGMA wal_autocheckpoint = 1000;", nullptr, nullptr, nullptr);
    sqlite3_exec(writer_db_, "PRAGMA cache_size = -16000;", nullptr, nullptr, nullptr);  // 16MB cache
    CheckpointWalUnlocked();
  }
  return true;
}

void StorageEngine::SetLastError(std::string msg) {
  std::lock_guard<std::mutex> lock(error_mutex_);
  last_error_ = std::move(msg);
}

std::string StorageEngine::LastErrorCopy() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return last_error_;
}

void StorageEngine::ClearLastError() {
  std::lock_guard<std::mutex> lock(error_mutex_);
  last_error_.clear();
}

std::string StorageEngine::DoGetLastError() const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  std::string error = LastErrorCopy();
  if (!error.empty()) {
    return error;
  }
  if (writer_db_) {
    return sqlite3_errmsg(writer_db_);
  }
  return "Database not open";
}

bool StorageEngine::DoOpen(const std::string &db_path, const std::string &key, const std::string &dict_dir) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  CloseUnlocked();
  ClearLastError();
  db_path_ = db_path;
  key_ = key;
  dict_dir_ = dict_dir;

  try {
    writer_cpp_db_ = std::make_unique<SQLite::Database>(db_path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    writer_db_ = writer_cpp_db_->getHandle();
  } catch (const std::exception &e) {
    SetLastError(e.what());
    CloseUnlocked();
    return false;
  }

  if (!ExecutePragmas(writer_db_, key)) {
    CloseUnlocked();
    return false;
  }

  // Register FTS5 Jieba tokenizer before compiling FTS5 tables
  int rc = RegisterFts5JiebaTokenizer(writer_db_, dict_dir);
  if (rc != SQLITE_OK) {
    SetLastError("Failed to register FTS5 Jieba tokenizer on writer: " + std::string(sqlite3_errmsg(writer_db_)));
    CloseUnlocked();
    return false;
  }

  if (!InitTables()) {
    CloseUnlocked();
    return false;
  }

  // Pre-initialize ReaderConnectionPool with 4 read-only connections
  if (!reader_pool_.Init(db_path, key, dict_dir, 4)) {
    LOG_WARN("[StorageEngine] ReaderConnectionPool failed to initialize: {}. Falling back to single-writer read.",
             reader_pool_.GetLastError());
  }

  return true;
}

bool StorageEngine::ExecutePragmas(sqlite3 *db, const std::string &key) {
  char *err_msg = nullptr;

  // Configure SQLite3MultipleCiphers for AES-128-CBC and 1 KDF iteration
  if (sqlite3_exec(db, "PRAGMA cipher = 'aes128cbc';", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    SetLastError(err_msg ? err_msg : "PRAGMA cipher failed");
    sqlite3_free(err_msg);
    return false;
  }

  if (sqlite3_exec(db, "PRAGMA kdf_iter = 1;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    SetLastError(err_msg ? err_msg : "PRAGMA kdf_iter failed");
    sqlite3_free(err_msg);
    return false;
  }

  // Set encryption key (escape single quotes for SQL literal safety)
  std::string escaped_key;
  escaped_key.reserve(key.size());
  for (char c : key) {
    if (c == '\'') escaped_key.push_back('\'');
    escaped_key.push_back(c);
  }
  std::string key_sql = "PRAGMA key = '" + escaped_key + "';";
  if (sqlite3_exec(db, key_sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
    SetLastError(err_msg ? err_msg : "PRAGMA key failed");
    sqlite3_free(err_msg);
    return false;
  }

  // Set large 32KB page size for high FTS5 indexing throughput
  sqlite3_exec(db, "PRAGMA page_size = 32768;", nullptr, nullptr, nullptr);

  // Configure incremental auto-vacuum before tables creation
  sqlite3_exec(db, "PRAGMA auto_vacuum = INCREMENTAL;", nullptr, nullptr, nullptr);

  // Verify password by testing access to schema
  sqlite3_stmt *check_stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master;", -1, &check_stmt, nullptr);
  if (rc != SQLITE_OK) {
    SetLastError("Database key verification failed: " + std::string(sqlite3_errmsg(db)));
    return false;
  }
  sqlite3_step(check_stmt);
  sqlite3_finalize(check_stmt);

  // Enable WAL mode, normal synchronous, memory temp store, mmap
  sqlite3_exec(db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA mmap_size = 268435456;", nullptr, nullptr, nullptr);
  sqlite3_busy_timeout(db, 5000);

  return true;
}

bool StorageEngine::InitTables() {
  const char *schema_sql = R"SQL(
    -- 1. Metadata and quick file name lookup
    CREATE TABLE IF NOT EXISTS files (
        doc_id      INTEGER PRIMARY KEY,
        root_dir_id INTEGER NOT NULL DEFAULT 0,
        parent_id   INTEGER NOT NULL DEFAULT 0,
        file_name   TEXT NOT NULL,
        file_path   TEXT NOT NULL UNIQUE,
        ext         TEXT NOT NULL,
        file_size   INTEGER NOT NULL DEFAULT 0,
        mtime       INTEGER NOT NULL DEFAULT 0,
        parse_level INTEGER NOT NULL DEFAULT 0,
        parse_stat  INTEGER NOT NULL DEFAULT 0
    );
    CREATE INDEX IF NOT EXISTS idx_files_ext_mtime ON files(ext, mtime);
    CREATE INDEX IF NOT EXISTS idx_files_name ON files(file_name COLLATE NOCASE);
    CREATE INDEX IF NOT EXISTS idx_files_path ON files(file_path);

    -- 2. Compressed original content storage
    CREATE TABLE IF NOT EXISTS doc_contents (
        doc_id      INTEGER PRIMARY KEY,
        raw_size    INTEGER NOT NULL,
        summary     TEXT,
        zstd_body   BLOB NOT NULL
    );

    -- 3. FTS5 full-text inverted index using custom jieba tokenizer
    CREATE VIRTUAL TABLE IF NOT EXISTS fts_docs USING fts5(
        file_name,
        body,
        content='',
        contentless_delete=1,
        tokenize='jieba'
    );

    -- 4. Ext stats table
    CREATE TABLE IF NOT EXISTS ext_stats (
        ext          TEXT PRIMARY KEY NOT NULL,
        active_count INTEGER NOT NULL DEFAULT 0
    );
  )SQL";

  char *err_msg = nullptr;
  int rc = sqlite3_exec(writer_db_, schema_sql, nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    SetLastError(err_msg ? err_msg : "Schema initialization failed");
    sqlite3_free(err_msg);
    return false;
  }
  return true;
}

bool StorageEngine::DoIndexDocument(const DocumentIndexEntry &entry) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    SetLastError("Database not open");
    return false;
  }

  // 1. Compress utf8_content with Zstd (level 3)
  size_t raw_size = entry.utf8_content.size();
  size_t max_compressed_size = ZSTD_compressBound(raw_size);
  std::vector<uint8_t> compressed(max_compressed_size);

  size_t compressed_size =
      ZSTD_compress(compressed.data(), max_compressed_size, entry.utf8_content.data(), raw_size, 3);
  if (ZSTD_isError(compressed_size)) {
    SetLastError("Zstd compression error: " + std::string(ZSTD_getErrorName(compressed_size)));
    return false;
  }
  compressed.resize(compressed_size);

  // Extract preliminary preview summary (up to first 200 chars)
  std::string summary;
  if (raw_size > 0) {
    size_t summary_len = std::min<size_t>(raw_size, 400);
    summary.assign(entry.utf8_content.data(), summary_len);
    if (!utf8::is_valid(summary.begin(), summary.end())) {
      std::string cleaned;
      utf8::replace_invalid(summary.begin(), summary.end(), std::back_inserter(cleaned));
      summary = std::move(cleaned);
    }
  }

  bool is_outer_tx = IsInTransactionUnlocked();
  if (!is_outer_tx) {
    sqlite3_exec(writer_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
  }

  // 2. Insert into files table
  if (!stmt_files_insert_) {
    const char *files_sql = R"SQL(
      INSERT INTO files (doc_id, file_name, file_path, ext, file_size, mtime, parse_level, parse_stat)
      VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 1)
      ON CONFLICT(doc_id) DO UPDATE SET
          file_name=excluded.file_name,
          file_path=excluded.file_path,
          ext=excluded.ext,
          file_size=excluded.file_size,
          mtime=excluded.mtime,
          parse_level=excluded.parse_level,
          parse_stat=excluded.parse_stat;
    )SQL";
    if (sqlite3_prepare_v2(writer_db_, files_sql, -1, &stmt_files_insert_, nullptr) != SQLITE_OK) {
      SetLastError(sqlite3_errmsg(writer_db_));
      if (!is_outer_tx) sqlite3_exec(writer_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  }

  sqlite3_reset(stmt_files_insert_);
  sqlite3_clear_bindings(stmt_files_insert_);
  sqlite3_bind_int64(stmt_files_insert_, 1, entry.doc_id);
  sqlite3_bind_text(stmt_files_insert_, 2, entry.file_name.c_str(), -1, SQLITE_TRANSIENT);
  std::string norm_file_path = entry.file_path;
  for (char &c : norm_file_path) {
    if (c == '\\') c = '/';
  }
  sqlite3_bind_text(stmt_files_insert_, 3, norm_file_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_files_insert_, 4, entry.ext.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt_files_insert_, 5, static_cast<int64_t>(entry.file_size));
  sqlite3_bind_int64(stmt_files_insert_, 6, entry.mtime);
  sqlite3_bind_int(stmt_files_insert_, 7, entry.parse_level);

  if (sqlite3_step(stmt_files_insert_) != SQLITE_DONE) {
    SetLastError(sqlite3_errmsg(writer_db_));
    sqlite3_reset(stmt_files_insert_);
    if (!is_outer_tx) sqlite3_exec(writer_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_reset(stmt_files_insert_);

  // 3. Insert into doc_contents table
  if (!stmt_contents_insert_) {
    const char *contents_sql = R"SQL(
      INSERT INTO doc_contents (doc_id, raw_size, summary, zstd_body)
      VALUES (?1, ?2, ?3, ?4)
      ON CONFLICT(doc_id) DO UPDATE SET
          raw_size=excluded.raw_size,
          summary=excluded.summary,
          zstd_body=excluded.zstd_body;
    )SQL";
    if (sqlite3_prepare_v2(writer_db_, contents_sql, -1, &stmt_contents_insert_, nullptr) != SQLITE_OK) {
      SetLastError(sqlite3_errmsg(writer_db_));
      if (!is_outer_tx) sqlite3_exec(writer_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  }

  sqlite3_reset(stmt_contents_insert_);
  sqlite3_clear_bindings(stmt_contents_insert_);
  sqlite3_bind_int64(stmt_contents_insert_, 1, entry.doc_id);
  sqlite3_bind_int64(stmt_contents_insert_, 2, static_cast<int64_t>(raw_size));
  sqlite3_bind_text(stmt_contents_insert_, 3, summary.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt_contents_insert_, 4, compressed.data(), static_cast<int>(compressed.size()), SQLITE_TRANSIENT);

  if (sqlite3_step(stmt_contents_insert_) != SQLITE_DONE) {
    SetLastError(sqlite3_errmsg(writer_db_));
    sqlite3_reset(stmt_contents_insert_);
    if (!is_outer_tx) sqlite3_exec(writer_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_reset(stmt_contents_insert_);

  // 4. Update fts_docs table (delete old rowid if exists, then insert)
  if (!stmt_fts_del_) {
    const char *fts_del_sql = "DELETE FROM fts_docs WHERE rowid = ?1;";
    sqlite3_prepare_v2(writer_db_, fts_del_sql, -1, &stmt_fts_del_, nullptr);
  }
  if (stmt_fts_del_) {
    sqlite3_reset(stmt_fts_del_);
    sqlite3_clear_bindings(stmt_fts_del_);
    sqlite3_bind_int64(stmt_fts_del_, 1, entry.doc_id);
    sqlite3_step(stmt_fts_del_);
    sqlite3_reset(stmt_fts_del_);
  }

  if (!stmt_fts_ins_) {
    const char *fts_ins_sql = "INSERT INTO fts_docs (rowid, file_name, body) VALUES (?1, ?2, ?3);";
    if (sqlite3_prepare_v2(writer_db_, fts_ins_sql, -1, &stmt_fts_ins_, nullptr) != SQLITE_OK) {
      SetLastError(sqlite3_errmsg(writer_db_));
      if (!is_outer_tx) sqlite3_exec(writer_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  }

  sqlite3_reset(stmt_fts_ins_);
  sqlite3_clear_bindings(stmt_fts_ins_);
  sqlite3_bind_int64(stmt_fts_ins_, 1, entry.doc_id);
  sqlite3_bind_text(stmt_fts_ins_, 2, entry.file_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_fts_ins_, 3, entry.utf8_content.c_str(), -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt_fts_ins_) != SQLITE_DONE) {
    SetLastError(sqlite3_errmsg(writer_db_));
    sqlite3_reset(stmt_fts_ins_);
    if (!is_outer_tx) sqlite3_exec(writer_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_reset(stmt_fts_ins_);

  if (is_outer_tx) {
    uncommitted_count_++;
  } else {
    sqlite3_exec(writer_db_, "COMMIT;", nullptr, nullptr, nullptr);
  }
  return true;
}

bool StorageEngine::DoDeleteDocument(int64_t doc_id) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    return false;
  }

  bool is_outer_tx = IsInTransactionUnlocked();
  if (!is_outer_tx) {
    sqlite3_exec(writer_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
  }

  if (writer_cpp_db_) {
    try {
      SQLite::Statement stmt1(*writer_cpp_db_, "DELETE FROM files WHERE doc_id = ?1;");
      stmt1.bind(1, doc_id);
      stmt1.exec();

      SQLite::Statement stmt2(*writer_cpp_db_, "DELETE FROM doc_contents WHERE doc_id = ?1;");
      stmt2.bind(1, doc_id);
      stmt2.exec();

      SQLite::Statement stmt3(*writer_cpp_db_, "DELETE FROM fts_docs WHERE rowid = ?1;");
      stmt3.bind(1, doc_id);
      stmt3.exec();
    } catch (const std::exception &e) {
      SetLastError(e.what());
      if (!is_outer_tx) sqlite3_exec(writer_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  } else {
    SqliteStmtScope stmt;
    sqlite3_prepare_v2(writer_db_, "DELETE FROM files WHERE doc_id = ?1;", -1, stmt.address(), nullptr);
    if (stmt) {
      sqlite3_bind_int64(stmt, 1, doc_id);
      sqlite3_step(stmt);
    }

    sqlite3_prepare_v2(writer_db_, "DELETE FROM doc_contents WHERE doc_id = ?1;", -1, stmt.address(), nullptr);
    if (stmt) {
      sqlite3_bind_int64(stmt, 1, doc_id);
      sqlite3_step(stmt);
    }

    sqlite3_prepare_v2(writer_db_, "DELETE FROM fts_docs WHERE rowid = ?1;", -1, stmt.address(), nullptr);
    if (stmt) {
      sqlite3_bind_int64(stmt, 1, doc_id);
      sqlite3_step(stmt);
    }
  }

  if (is_outer_tx) {
    uncommitted_count_++;
  } else {
    sqlite3_exec(writer_db_, "COMMIT;", nullptr, nullptr, nullptr);
  }
  return true;
}

int64_t StorageEngine::DoDeleteDirectory(const std::string &dir_path, std::vector<int64_t> *out_deleted_doc_ids) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    SetLastError("Database not open");
    return -1;
  }
  if (dir_path.empty()) {
    return 0;
  }

  std::string normalized = dir_path;
  for (char &c : normalized) {
    if (c == '\\') c = '/';
  }
  while (normalized.size() > 1 && (normalized.back() == '/' || normalized.back() == '\\')) {
    normalized.pop_back();
  }
  std::string escaped = EscapeLikePattern(normalized);
  std::string prefix_slash = escaped + "/%";
  std::string prefix_bslash = escaped + "\\%";

  // 1. Query all matching doc_ids and extensions with ESCAPE clause
  const char *query_sql =
      "SELECT doc_id, ext FROM files WHERE file_path = ?1 OR file_path LIKE ?2 ESCAPE '^' OR file_path LIKE ?3 ESCAPE "
      "'^';";
  SqliteStmtScope stmt_select;
  int rc = sqlite3_prepare_v2(writer_db_, query_sql, -1, stmt_select.address(), nullptr);
  if (rc != SQLITE_OK) {
    SetLastError(sqlite3_errmsg(writer_db_));
    return -1;
  }

  sqlite3_bind_text(stmt_select, 1, normalized.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_select, 2, prefix_slash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_select, 3, prefix_bslash.c_str(), -1, SQLITE_TRANSIENT);

  std::vector<int64_t> doc_ids;
  std::unordered_map<std::string, int> ext_counts;
  while (sqlite3_step(stmt_select) == SQLITE_ROW) {
    int64_t id = sqlite3_column_int64(stmt_select, 0);
    doc_ids.push_back(id);
    const char *ext = reinterpret_cast<const char *>(sqlite3_column_text(stmt_select, 1));
    if (ext) {
      ext_counts[ext]++;
    }
  }
  stmt_select.Reset();

  if (doc_ids.empty()) {
    if (out_deleted_doc_ids) {
      out_deleted_doc_ids->clear();
    }
    return 0;
  }

  // 2. Cascade delete within a single transaction
  bool is_outer_tx = IsInTransactionUnlocked();
  if (!is_outer_tx) {
    sqlite3_exec(writer_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
  }

  SqliteStmtScope stmt_del_content;
  SqliteStmtScope stmt_del_fts;
  sqlite3_prepare_v2(writer_db_, "DELETE FROM doc_contents WHERE doc_id = ?1;", -1, stmt_del_content.address(),
                     nullptr);
  sqlite3_prepare_v2(writer_db_, "DELETE FROM fts_docs WHERE rowid = ?1;", -1, stmt_del_fts.address(), nullptr);

  for (int64_t id : doc_ids) {
    if (stmt_del_content) {
      sqlite3_bind_int64(stmt_del_content, 1, id);
      sqlite3_step(stmt_del_content);
      sqlite3_reset(stmt_del_content);
    }
    if (stmt_del_fts) {
      sqlite3_bind_int64(stmt_del_fts, 1, id);
      sqlite3_step(stmt_del_fts);
      sqlite3_reset(stmt_del_fts);
    }
  }
  stmt_del_content.Reset();
  stmt_del_fts.Reset();

  // 3. Batch delete from files table
  const char *del_files_sql =
      "DELETE FROM files WHERE file_path = ?1 OR file_path LIKE ?2 ESCAPE '^' OR file_path LIKE ?3 ESCAPE '^';";
  SqliteStmtScope stmt_del_files;
  if (sqlite3_prepare_v2(writer_db_, del_files_sql, -1, stmt_del_files.address(), nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt_del_files, 1, normalized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_del_files, 2, prefix_slash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_del_files, 3, prefix_bslash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt_del_files);
  }

  // 4. Update ext_stats active_count
  const char *update_stat_sql = "UPDATE ext_stats SET active_count = MAX(0, active_count - ?1) WHERE ext = ?2;";
  SqliteStmtScope stmt_update_stat;
  if (sqlite3_prepare_v2(writer_db_, update_stat_sql, -1, stmt_update_stat.address(), nullptr) == SQLITE_OK) {
    for (const auto &pair : ext_counts) {
      sqlite3_bind_int(stmt_update_stat, 1, pair.second);
      sqlite3_bind_text(stmt_update_stat, 2, pair.first.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt_update_stat);
      sqlite3_reset(stmt_update_stat);
    }
  }

  if (is_outer_tx) {
    uncommitted_count_ += doc_ids.size();
  } else {
    sqlite3_exec(writer_db_, "COMMIT;", nullptr, nullptr, nullptr);
  }

  if (out_deleted_doc_ids) {
    *out_deleted_doc_ids = std::move(doc_ids);
    return static_cast<int64_t>(out_deleted_doc_ids->size());
  }
  return static_cast<int64_t>(doc_ids.size());
}

bool StorageEngine::DoClearAll() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!writer_db_) {
    return false;
  }

  FinalizeCachedStatements();

  if (writer_cpp_db_) {
    try {
      SQLite::Transaction transaction(*writer_cpp_db_);
      writer_cpp_db_->exec("DELETE FROM files;");
      writer_cpp_db_->exec("DELETE FROM doc_contents;");
      writer_cpp_db_->exec("DELETE FROM ext_stats;");
      writer_cpp_db_->exec("DROP TABLE IF EXISTS fts_docs;");
      transaction.commit();
      uncommitted_count_ = 0;
      writer_cpp_db_->exec("VACUUM;");
      return InitTables();
    } catch (const std::exception &e) {
      SetLastError(e.what());
      return false;
    }
  }

  sqlite3_exec(writer_db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);

  char *err_msg = nullptr;
  const char *sql = R"SQL(
    DELETE FROM files;
    DELETE FROM doc_contents;
    DELETE FROM ext_stats;
    DROP TABLE IF EXISTS fts_docs;
  )SQL";

  int rc = sqlite3_exec(writer_db_, sql, nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    SetLastError(err_msg ? err_msg : "ClearAll failed");
    sqlite3_free(err_msg);
    sqlite3_exec(writer_db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_exec(writer_db_, "COMMIT;", nullptr, nullptr, nullptr);
  uncommitted_count_ = 0;

  // Reclaim all unused free pages to operating system and truncate database file
  sqlite3_exec(writer_db_, "VACUUM;", nullptr, nullptr, nullptr);
  sqlite3_exec(writer_db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);

  return InitTables();
}

ScopedReadConnection StorageEngine::DoAcquireReadConnection() {
  ScopedReadConnection conn = reader_pool_.Acquire(5000);
  return conn;
}

std::vector<SearchResultItem> StorageEngine::DoSearch(const std::string &query, int limit, int offset,
                                                      const std::string &path_prefix, int *out_total_count) {
  std::vector<SearchResultItem> results;
  if (out_total_count) {
    *out_total_count = 0;
  }
  if (query.empty()) {
    return results;
  }

  // Acquire read connection from pool (Zero lock on writer_mutex_!)
  ScopedReadConnection read_conn = AcquireReadConnection();
  sqlite3 *target_db = read_conn.get();
  std::unique_lock<std::mutex> fallback_lock;
  if (!target_db) {
    fallback_lock = std::unique_lock<std::mutex>(writer_mutex_);
    target_db = writer_db_;
  }
  if (!target_db) {
    return results;
  }

  bool has_prefix = !path_prefix.empty();
  std::string prefix_pattern;
  if (has_prefix) {
    std::string norm_prefix = path_prefix;
    for (char &c : norm_prefix) {
      if (c == '\\') c = '/';
    }
    prefix_pattern = EscapeLikePattern(norm_prefix) + "%";
  }

  const char *search_sql = has_prefix ? R"SQL(
          SELECT fts.rowid, f.file_name, f.file_path, f.ext, f.file_size, f.mtime, bm25(fts_docs) AS score
          FROM fts_docs AS fts
          JOIN files AS f ON f.doc_id = fts.rowid
          WHERE fts_docs MATCH ?1 AND f.file_path LIKE ?4 ESCAPE '^'
          ORDER BY score ASC
          LIMIT ?2 OFFSET ?3;
        )SQL"
                                      : R"SQL(
          SELECT fts.rowid, f.file_name, f.file_path, f.ext, f.file_size, f.mtime, bm25(fts_docs) AS score
          FROM fts_docs AS fts
          JOIN files AS f ON f.doc_id = fts.rowid
          WHERE fts_docs MATCH ?1
          ORDER BY score ASC
          LIMIT ?2 OFFSET ?3;
        )SQL";

  auto execute_search = [&](const std::string &q, bool is_retry) -> bool {
    // 1. Calculate total matched count if requested
    if (out_total_count) {
      const char *count_sql = has_prefix
                                  ? "SELECT count(*) FROM fts_docs AS fts JOIN files AS f ON f.doc_id = fts.rowid "
                                    "WHERE fts_docs MATCH ?1 AND f.file_path LIKE ?2 ESCAPE '^';"
                                  : "SELECT count(*) FROM fts_docs WHERE fts_docs MATCH ?1;";
      SqliteStmtScope count_stmt;
      if (sqlite3_prepare_v2(target_db, count_sql, -1, count_stmt.address(), nullptr) == SQLITE_OK) {
        sqlite3_bind_text(count_stmt, 1, q.c_str(), -1, SQLITE_TRANSIENT);
        if (has_prefix) {
          sqlite3_bind_text(count_stmt, 2, prefix_pattern.c_str(), -1, SQLITE_TRANSIENT);
        }
        int rc = sqlite3_step(count_stmt);
        if (rc == SQLITE_ROW) {
          *out_total_count = sqlite3_column_int(count_stmt, 0);
        } else if (rc != SQLITE_DONE && !is_retry) {
          return false;
        }
      } else if (!is_retry) {
        return false;
      }
    }

    // 2. Query search rows
    SqliteStmtScope stmt;
    if (sqlite3_prepare_v2(target_db, search_sql, -1, stmt.address(), nullptr) != SQLITE_OK) {
      if (!is_retry) return false;
      SetLastError(sqlite3_errmsg(target_db));
      return true;
    }

    sqlite3_bind_text(stmt, 1, q.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);
    if (has_prefix) {
      sqlite3_bind_text(stmt, 4, prefix_pattern.c_str(), -1, SQLITE_TRANSIENT);
    }

    while (true) {
      int rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        SearchResultItem item;
        item.doc_id = sqlite3_column_int64(stmt, 0);
        item.file_name = SafeColumnText(stmt, 1);
        item.file_path = SafeColumnText(stmt, 2);
        item.ext = SafeColumnText(stmt, 3);
        item.file_size = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
        item.mtime = sqlite3_column_int64(stmt, 5);
        item.rank = sqlite3_column_double(stmt, 6);
        results.push_back(std::move(item));
      } else if (rc == SQLITE_DONE) {
        break;
      } else {
        if (!is_retry) {
          results.clear();
          return false;
        }
        SetLastError(sqlite3_errmsg(target_db));
        break;
      }
    }
    return true;
  };

  if (!execute_search(query, false)) {
    // Primary query failed due to FTS5 syntax error (e.g. C++, lock-free, unclosed quotes, pure operators).
    // Gracefully downgrade to sanitized literal token phrase search.
    std::string fallback_query = BuildSanitizedFtsQuery(query);
    if (!fallback_query.empty() && fallback_query != query) {
      results.clear();
      if (out_total_count) *out_total_count = 0;
      execute_search(fallback_query, true);
    }
  }

  return results;
}

bool StorageEngine::DoGetDocumentContent(int64_t doc_id, std::string *out_content) {
  ScopedReadConnection read_conn = AcquireReadConnection();
  sqlite3 *target_db = read_conn.get();
  std::unique_lock<std::mutex> fallback_lock;
  if (!target_db) {
    fallback_lock = std::unique_lock<std::mutex>(writer_mutex_);
    target_db = writer_db_;
  }
  return GetDocumentContentOnDb(target_db, doc_id, out_content);
}

bool StorageEngine::GetDocumentContentOnDb(sqlite3 *db, int64_t doc_id, std::string *out_content) {
  if (!out_content) {
    SetLastError("Output pointer is null");
    return false;
  }
  if (!db) {
    SetLastError("Database not open");
    return false;
  }

  const char *sql = "SELECT raw_size, zstd_body FROM doc_contents WHERE doc_id = ?1;";
  SqliteStmtScope stmt;
  if (sqlite3_prepare_v2(db, sql, -1, stmt.address(), nullptr) != SQLITE_OK) {
    SetLastError(sqlite3_errmsg(db));
    return false;
  }

  sqlite3_bind_int64(stmt, 1, doc_id);

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    SetLastError("Document content not found");
    return false;
  }

  const int64_t raw_size_i64 = sqlite3_column_int64(stmt, 0);
  const void *blob_data = sqlite3_column_blob(stmt, 1);
  int blob_bytes = sqlite3_column_bytes(stmt, 1);

  // Guard against corrupted/overflowed metadata: never trust the stored size for allocation.
  constexpr int64_t kMaxDecompressedDocBytes = 512LL * 1024LL * 1024LL;
  if (raw_size_i64 < 0 || raw_size_i64 > kMaxDecompressedDocBytes) {
    SetLastError("Document content size out of range: " + std::to_string(raw_size_i64));
    out_content->clear();
    return false;
  }

  if (!blob_data || blob_bytes <= 0 || raw_size_i64 == 0) {
    out_content->clear();
    return true;
  }

  const size_t raw_size = static_cast<size_t>(raw_size_i64);
  out_content->resize(raw_size);
  size_t decompressed_size = ZSTD_decompress(&(*out_content)[0], raw_size, blob_data, static_cast<size_t>(blob_bytes));

  if (ZSTD_isError(decompressed_size)) {
    SetLastError("Zstd decompression error: " + std::string(ZSTD_getErrorName(decompressed_size)));
    out_content->clear();
    return false;
  }

  // ZSTD_decompress may return fewer bytes than the stored raw_size for legacy/corrupt rows;
  // shrink so callers never see trailing NUL padding.
  if (decompressed_size != raw_size) {
    LOG_WARN("[StorageEngine] Decompressed size mismatch for doc {}: stored {}, actual {}", doc_id, raw_size,
             decompressed_size);
    out_content->resize(decompressed_size);
  }

  return true;
}

bool StorageEngine::DoGetFileMetadata(int64_t doc_id, uint64_t *out_file_size, int64_t *out_mtime,
                                      int *out_parse_stat) {
  ScopedReadConnection read_conn = AcquireReadConnection();
  sqlite3 *target_db = read_conn.get();
  std::unique_lock<std::mutex> fallback_lock;
  if (!target_db) {
    fallback_lock = std::unique_lock<std::mutex>(writer_mutex_);
    target_db = writer_db_;
  }
  if (!target_db) {
    SetLastError("Database not open");
    return false;
  }

  const char *sql = "SELECT file_size, mtime, parse_stat FROM files WHERE doc_id = ?;";
  SqliteStmtScope stmt;
  if (sqlite3_prepare_v2(target_db, sql, -1, stmt.address(), nullptr) != SQLITE_OK) {
    SetLastError(sqlite3_errmsg(target_db));
    return false;
  }

  sqlite3_bind_int64(stmt, 1, doc_id);
  bool found = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    if (out_file_size) {
      *out_file_size = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    if (out_mtime) {
      *out_mtime = sqlite3_column_int64(stmt, 1);
    }
    if (out_parse_stat) {
      *out_parse_stat = sqlite3_column_int(stmt, 2);
    }
    found = true;
  }
  return found;
}

std::vector<FileMetaRecord> StorageEngine::DoGetAllFilesMetadata() {
  std::vector<FileMetaRecord> results;
  ScopedReadConnection read_conn = AcquireReadConnection();
  sqlite3 *target_db = read_conn.get();
  std::unique_lock<std::mutex> fallback_lock;
  if (!target_db) {
    fallback_lock = std::unique_lock<std::mutex>(writer_mutex_);
    target_db = writer_db_;
  }
  if (!target_db) {
    SetLastError("Database not open");
    return results;
  }

  const char *sql = "SELECT doc_id, file_name, file_path, ext, file_size, mtime, parse_level, parse_stat FROM files;";
  SqliteStmtScope stmt;
  if (sqlite3_prepare_v2(target_db, sql, -1, stmt.address(), nullptr) != SQLITE_OK) {
    SetLastError(sqlite3_errmsg(target_db));
    return results;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    FileMetaRecord r;
    r.doc_id = sqlite3_column_int64(stmt, 0);
    r.file_name = SafeColumnText(stmt, 1);
    r.file_path = SafeColumnText(stmt, 2);
    r.ext = SafeColumnText(stmt, 3);
    r.file_size = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
    r.mtime = sqlite3_column_int64(stmt, 5);
    r.parse_level = sqlite3_column_int(stmt, 6);
    r.parse_stat = sqlite3_column_int(stmt, 7);
    results.push_back(std::move(r));
  }
  return results;
}

}  // namespace storage
}  // namespace soeasy
