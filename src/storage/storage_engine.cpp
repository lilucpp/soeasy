#include "storage_engine.h"

#include <cctype>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "fts5_jieba_tokenizer.h"
#include "utf8.h"
#include "zstd.h"

namespace soeasy {
namespace storage {

std::vector<std::string> StorageEngine::ExtractKeywordsFromQuery(
    const std::string& query) {
  std::vector<std::string> kws;
  std::istringstream iss(query);
  std::string token;
  while (iss >> token) {
    if (token == "AND" || token == "OR" || token == "NOT") continue;
    if (token.rfind("NEAR(", 0) == 0) continue;
    size_t colon = token.find(':');
    if (colon != std::string::npos && colon + 1 < token.size()) {
      token = token.substr(colon + 1);
    }
    while (!token.empty() && (token.front() == '"' || token.front() == '\'' || token.front() == '(')) {
      token.erase(token.begin());
    }
    while (!token.empty() && (token.back() == '"' || token.back() == '\'' || token.back() == ')' || token.back() == '*')) {
      token.pop_back();
    }
    if (!token.empty()) {
      kws.push_back(token);
    }
  }
  return kws;
}

namespace {
}  // namespace

StorageEngine::StorageEngine() = default;

StorageEngine::~StorageEngine() {
  Close();
}

void StorageEngine::Close() {
  if (db_) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

std::string StorageEngine::GetLastError() const {
  if (!last_error_.empty()) {
    return last_error_;
  }
  if (db_) {
    return sqlite3_errmsg(db_);
  }
  return "Database not open";
}

bool StorageEngine::Open(const std::string& db_path, const std::string& key,
                         const std::string& dict_dir) {
  Close();
  last_error_.clear();

  int rc = sqlite3_open_v2(db_path.c_str(), &db_,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (rc != SQLITE_OK) {
    last_error_ = sqlite3_errmsg(db_);
    Close();
    return false;
  }

  if (!ExecutePragmas(key)) {
    Close();
    return false;
  }

  // Register FTS5 Jieba tokenizer before compiling FTS5 tables
  rc = RegisterFts5JiebaTokenizer(db_, dict_dir);
  if (rc != SQLITE_OK) {
    last_error_ = "Failed to register FTS5 Jieba tokenizer: " +
                  std::string(sqlite3_errmsg(db_));
    Close();
    return false;
  }

  if (!InitTables()) {
    Close();
    return false;
  }

  return true;
}

bool StorageEngine::ExecutePragmas(const std::string& key) {
  char* err_msg = nullptr;

  // Configure SQLite3MultipleCiphers for AES-128-CBC and 1 KDF iteration
  if (sqlite3_exec(db_, "PRAGMA cipher = 'aes128cbc';", nullptr, nullptr,
                   &err_msg) != SQLITE_OK) {
    last_error_ = err_msg ? err_msg : "PRAGMA cipher failed";
    sqlite3_free(err_msg);
    return false;
  }

  if (sqlite3_exec(db_, "PRAGMA kdf_iter = 1;", nullptr, nullptr,
                   &err_msg) != SQLITE_OK) {
    last_error_ = err_msg ? err_msg : "PRAGMA kdf_iter failed";
    sqlite3_free(err_msg);
    return false;
  }

  // Set encryption key
  std::string key_sql = "PRAGMA key = '" + key + "';";
  if (sqlite3_exec(db_, key_sql.c_str(), nullptr, nullptr,
                   &err_msg) != SQLITE_OK) {
    last_error_ = err_msg ? err_msg : "PRAGMA key failed";
    sqlite3_free(err_msg);
    return false;
  }

  // Set large 32KB page size for high FTS5 indexing throughput
  sqlite3_exec(db_, "PRAGMA page_size = 32768;", nullptr, nullptr, nullptr);

  // Verify password by testing access to schema
  sqlite3_stmt* check_stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, "SELECT count(*) FROM sqlite_master;", -1,
                              &check_stmt, nullptr);
  if (rc != SQLITE_OK) {
    last_error_ = "Database key verification failed: " +
                  std::string(sqlite3_errmsg(db_));
    return false;
  }
  sqlite3_step(check_stmt);
  sqlite3_finalize(check_stmt);

  // Enable WAL mode, normal synchronous, memory temp store, mmap
  sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "PRAGMA mmap_size = 268435456;", nullptr, nullptr, nullptr);

  return true;
}

bool StorageEngine::InitTables() {
  const char* schema_sql = R"SQL(
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

  char* err_msg = nullptr;
  int rc = sqlite3_exec(db_, schema_sql, nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    last_error_ = err_msg ? err_msg : "Schema initialization failed";
    sqlite3_free(err_msg);
    return false;
  }
  return true;
}

bool StorageEngine::IndexDocument(int64_t doc_id,
                                  const std::string& file_path,
                                  const std::string& file_name,
                                  const std::string& ext,
                                  uint64_t file_size,
                                  int64_t mtime,
                                  int parse_level,
                                  const std::string& utf8_content) {
  if (!db_) {
    last_error_ = "Database not open";
    return false;
  }

  // 1. Compress utf8_content with Zstd (level 3)
  size_t raw_size = utf8_content.size();
  size_t max_compressed_size = ZSTD_compressBound(raw_size);
  std::vector<uint8_t> compressed(max_compressed_size);

  size_t compressed_size = ZSTD_compress(
      compressed.data(), max_compressed_size, utf8_content.data(), raw_size, 3);
  if (ZSTD_isError(compressed_size)) {
    last_error_ = "Zstd compression error: " +
                  std::string(ZSTD_getErrorName(compressed_size));
    return false;
  }
  compressed.resize(compressed_size);

  // Extract preliminary preview summary (up to first 200 chars)
  std::string summary;
  if (raw_size > 0) {
    size_t summary_len = std::min<size_t>(raw_size, 400);
    summary.assign(utf8_content.data(), summary_len);
    if (!utf8::is_valid(summary.begin(), summary.end())) {
      std::string cleaned;
      utf8::replace_invalid(summary.begin(), summary.end(),
                            std::back_inserter(cleaned));
      summary = std::move(cleaned);
    }
  }

  // Begin single transaction
  sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  // 2. Insert into files table
  const char* files_sql = R"SQL(
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

  sqlite3_stmt* stmt_files = nullptr;
  if (sqlite3_prepare_v2(db_, files_sql, -1, &stmt_files, nullptr) != SQLITE_OK) {
    last_error_ = sqlite3_errmsg(db_);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_bind_int64(stmt_files, 1, doc_id);
  sqlite3_bind_text(stmt_files, 2, file_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_files, 3, file_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_files, 4, ext.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt_files, 5, static_cast<int64_t>(file_size));
  sqlite3_bind_int64(stmt_files, 6, mtime);
  sqlite3_bind_int(stmt_files, 7, parse_level);

  if (sqlite3_step(stmt_files) != SQLITE_DONE) {
    last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt_files);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_finalize(stmt_files);

  // 3. Insert into doc_contents table
  const char* contents_sql = R"SQL(
    INSERT INTO doc_contents (doc_id, raw_size, summary, zstd_body)
    VALUES (?1, ?2, ?3, ?4)
    ON CONFLICT(doc_id) DO UPDATE SET
        raw_size=excluded.raw_size,
        summary=excluded.summary,
        zstd_body=excluded.zstd_body;
  )SQL";

  sqlite3_stmt* stmt_contents = nullptr;
  if (sqlite3_prepare_v2(db_, contents_sql, -1, &stmt_contents, nullptr) !=
      SQLITE_OK) {
    last_error_ = sqlite3_errmsg(db_);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_bind_int64(stmt_contents, 1, doc_id);
  sqlite3_bind_int64(stmt_contents, 2, static_cast<int64_t>(raw_size));
  sqlite3_bind_text(stmt_contents, 3, summary.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt_contents, 4, compressed.data(),
                    static_cast<int>(compressed.size()), SQLITE_TRANSIENT);

  if (sqlite3_step(stmt_contents) != SQLITE_DONE) {
    last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt_contents);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_finalize(stmt_contents);

  // 4. Update fts_docs table (delete old rowid if exists, then insert)
  const char* fts_del_sql = "DELETE FROM fts_docs WHERE rowid = ?1;";
  sqlite3_stmt* stmt_fts_del = nullptr;
  if (sqlite3_prepare_v2(db_, fts_del_sql, -1, &stmt_fts_del, nullptr) ==
      SQLITE_OK) {
    sqlite3_bind_int64(stmt_fts_del, 1, doc_id);
    sqlite3_step(stmt_fts_del);
    sqlite3_finalize(stmt_fts_del);
  }

  const char* fts_ins_sql =
      "INSERT INTO fts_docs (rowid, file_name, body) VALUES (?1, ?2, ?3);";
  sqlite3_stmt* stmt_fts_ins = nullptr;
  if (sqlite3_prepare_v2(db_, fts_ins_sql, -1, &stmt_fts_ins, nullptr) !=
      SQLITE_OK) {
    last_error_ = sqlite3_errmsg(db_);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_bind_int64(stmt_fts_ins, 1, doc_id);
  sqlite3_bind_text(stmt_fts_ins, 2, file_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt_fts_ins, 3, utf8_content.c_str(), -1,
                    SQLITE_TRANSIENT);

  if (sqlite3_step(stmt_fts_ins) != SQLITE_DONE) {
    last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt_fts_ins);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_finalize(stmt_fts_ins);

  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

bool StorageEngine::DeleteDocument(int64_t doc_id) {
  if (!db_) {
    return false;
  }

  sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_, "DELETE FROM files WHERE doc_id = ?1;", -1, &stmt,
                     nullptr);
  sqlite3_bind_int64(stmt, 1, doc_id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  sqlite3_prepare_v2(db_, "DELETE FROM doc_contents WHERE doc_id = ?1;", -1,
                     &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, doc_id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  sqlite3_prepare_v2(db_, "DELETE FROM fts_docs WHERE rowid = ?1;", -1, &stmt,
                     nullptr);
  sqlite3_bind_int64(stmt, 1, doc_id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

bool StorageEngine::ClearAll() {
  if (!db_) {
    return false;
  }

  sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  char* err_msg = nullptr;
  const char* sql = R"SQL(
    DELETE FROM files;
    DELETE FROM doc_contents;
    DELETE FROM ext_stats;
    DROP TABLE IF EXISTS fts_docs;
  )SQL";

  int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    last_error_ = err_msg ? err_msg : "ClearAll failed";
    sqlite3_free(err_msg);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return InitTables();
}


std::vector<SearchResultItem> StorageEngine::Search(const std::string& query,
                                                    int limit, int offset) {
  std::vector<SearchResultItem> results;
  if (!db_ || query.empty()) {
    return results;
  }

  const char* search_sql = R"SQL(
    SELECT fts.rowid, f.file_name, f.file_path, f.ext, f.file_size, f.mtime, bm25(fts_docs) AS score
    FROM fts_docs AS fts
    JOIN files AS f ON f.doc_id = fts.rowid
    WHERE fts_docs MATCH ?1
    ORDER BY score ASC
    LIMIT ?2 OFFSET ?3;
  )SQL";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, search_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    last_error_ = sqlite3_errmsg(db_);
    return results;
  }

  sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, limit);
  sqlite3_bind_int(stmt, 3, offset);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    SearchResultItem item;
    item.doc_id = sqlite3_column_int64(stmt, 0);
    item.file_name =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    item.file_path =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    item.ext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    item.file_size = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
    item.mtime = sqlite3_column_int64(stmt, 5);
    item.rank = sqlite3_column_double(stmt, 6);
    results.push_back(std::move(item));
  }

  sqlite3_finalize(stmt);

  // Extract snippet surrounding matched keywords from decompressed document body
  // Cap decompression to top 10 results to maintain lightning-fast keystroke latency
  auto keywords = ExtractKeywordsFromQuery(query);
  const size_t max_body_snippets = std::min(results.size(), size_t(10));
  for (size_t i = 0; i < results.size(); ++i) {
    auto& item = results[i];
    if (i < max_body_snippets) {
      std::string full_text;
      if (GetDocumentContent(item.doc_id, &full_text) && !full_text.empty()) {
        item.snippet = GenerateSnippet(full_text, keywords, 140);
        if (!item.snippet.empty()) {
          item.has_body_snippet = true;
        }
      }
    }
    if (item.snippet.empty()) {
      item.snippet = GenerateSnippet(item.file_name, keywords, 100);
      item.has_body_snippet = false;
    }
  }

  return results;
}

bool StorageEngine::GetDocumentContent(int64_t doc_id,
                                       std::string* out_content) {
  if (!out_content) {
    last_error_ = "Output pointer is null";
    return false;
  }
  if (!db_) {
    last_error_ = "Database not open";
    return false;
  }

  const char* sql =
      "SELECT raw_size, zstd_body FROM doc_contents WHERE doc_id = ?1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    last_error_ = sqlite3_errmsg(db_);
    return false;
  }

  sqlite3_bind_int64(stmt, 1, doc_id);

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    last_error_ = "Document content not found";
    sqlite3_finalize(stmt);
    return false;
  }

  size_t raw_size = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
  const void* blob_data = sqlite3_column_blob(stmt, 1);
  int blob_bytes = sqlite3_column_bytes(stmt, 1);

  if (!blob_data || blob_bytes <= 0 || raw_size == 0) {
    out_content->clear();
    sqlite3_finalize(stmt);
    return true;
  }

  out_content->resize(raw_size);
  size_t decompressed_size = ZSTD_decompress(
      &(*out_content)[0], raw_size, blob_data, static_cast<size_t>(blob_bytes));
  sqlite3_finalize(stmt);

  if (ZSTD_isError(decompressed_size)) {
    last_error_ = "Zstd decompression error: " +
                  std::string(ZSTD_getErrorName(decompressed_size));
    out_content->clear();
    return false;
  }

  return true;
}

bool StorageEngine::GetFileMetadata(int64_t doc_id, uint64_t* out_file_size,
                                    int64_t* out_mtime, int* out_parse_stat) {
  if (!db_) {
    last_error_ = "Database not open";
    return false;
  }

  const char* sql = "SELECT file_size, mtime, parse_stat FROM files WHERE doc_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    last_error_ = sqlite3_errmsg(db_);
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
  sqlite3_finalize(stmt);
  return found;
}

std::vector<FileMetaRecord> StorageEngine::GetAllFilesMetadata() {
  std::vector<FileMetaRecord> results;
  if (!db_) {
    last_error_ = "Database not open";
    return results;
  }

  const char* sql =
      "SELECT doc_id, file_name, file_path, ext, file_size, mtime, parse_level, parse_stat "
      "FROM files;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    last_error_ = sqlite3_errmsg(db_);
    return results;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    FileMetaRecord r;
    r.doc_id = sqlite3_column_int64(stmt, 0);
    const unsigned char* name = sqlite3_column_text(stmt, 1);
    if (name) r.file_name = reinterpret_cast<const char*>(name);
    const unsigned char* path = sqlite3_column_text(stmt, 2);
    if (path) r.file_path = reinterpret_cast<const char*>(path);
    const unsigned char* ext = sqlite3_column_text(stmt, 3);
    if (ext) r.ext = reinterpret_cast<const char*>(ext);
    r.file_size = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
    r.mtime = sqlite3_column_int64(stmt, 5);
    r.parse_level = sqlite3_column_int(stmt, 6);
    r.parse_stat = sqlite3_column_int(stmt, 7);
    results.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return results;
}

std::string StorageEngine::GenerateSnippet(
    const std::string& full_text,
    const std::vector<std::string>& keywords,
    size_t max_chars) {
  if (full_text.empty() || keywords.empty()) {
    return "";
  }

  // Lowercase version of full_text for case-insensitive search
  std::string lower_text = full_text;
  for (char& c : lower_text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }

  // Find the earliest matching keyword position
  size_t best_pos = std::string::npos;
  for (const auto& kw : keywords) {
    if (kw.empty()) continue;
    std::string lower_kw = kw;
    for (char& c : lower_kw) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    size_t pos = lower_text.find(lower_kw);
    if (pos != std::string::npos) {
      if (best_pos == std::string::npos || pos < best_pos) {
        best_pos = pos;
      }
    }
  }

  size_t start_pos = 0;
  size_t end_pos = full_text.size();

  if (best_pos != std::string::npos) {
    // Window around best_pos: reserve approx 1/3 lead characters before keyword
    size_t lead_chars = max_chars / 3;
    start_pos = (best_pos > lead_chars) ? (best_pos - lead_chars) : 0;
    end_pos = std::min(full_text.size(), start_pos + max_chars);

    // Adjust start_pos to valid UTF-8 character boundary
    while (start_pos > 0 &&
           (static_cast<unsigned char>(full_text[start_pos]) & 0xC0) == 0x80) {
      --start_pos;
    }
    // Adjust end_pos to valid UTF-8 character boundary
    while (end_pos < full_text.size() &&
           (static_cast<unsigned char>(full_text[end_pos]) & 0xC0) == 0x80) {
      ++end_pos;
    }
  } else {
    // No keyword found; take prefix of text
    end_pos = std::min(full_text.size(), max_chars);
    while (end_pos < full_text.size() &&
           (static_cast<unsigned char>(full_text[end_pos]) & 0xC0) == 0x80) {
      ++end_pos;
    }
  }

  std::string raw_snippet = full_text.substr(start_pos, end_pos - start_pos);

  // Validate UTF-8 encoding
  if (!utf8::is_valid(raw_snippet.begin(), raw_snippet.end())) {
    std::string cleaned;
    utf8::replace_invalid(raw_snippet.begin(), raw_snippet.end(),
                          std::back_inserter(cleaned));
    raw_snippet = std::move(cleaned);
  }

  std::string lower_snippet = raw_snippet;
  for (char& c : lower_snippet) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }

  struct Span {
    size_t start;
    size_t end;
  };
  std::vector<Span> spans;

  for (const auto& kw : keywords) {
    if (kw.empty()) continue;
    std::string lower_kw = kw;
    for (char& c : lower_kw) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }

    size_t cur = 0;
    while ((cur = lower_snippet.find(lower_kw, cur)) != std::string::npos) {
      spans.push_back({cur, cur + kw.size()});
      cur += kw.size();
    }
  }

  // Sort spans by start position ascending
  std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
    if (a.start != b.start) return a.start < b.start;
    return a.end > b.end;
  });

  // Merge overlapping or adjacent spans
  std::vector<Span> merged;
  for (const auto& s : spans) {
    if (merged.empty()) {
      merged.push_back(s);
    } else {
      if (s.start <= merged.back().end) {
        merged.back().end = std::max(merged.back().end, s.end);
      } else {
        merged.push_back(s);
      }
    }
  }

  auto EscapeHtml = [](const std::string& str) -> std::string {
    std::string out;
    out.reserve(str.size() + 16);
    for (char c : str) {
      switch (c) {
        case '&': out.append("&amp;"); break;
        case '<': out.append("&lt;"); break;
        case '>': out.append("&gt;"); break;
        case '"': out.append("&quot;"); break;
        default:  out.push_back(c); break;
      }
    }
    return out;
  };

  std::string formatted;
  size_t last_idx = 0;
  for (const auto& span : merged) {
    if (span.start > last_idx) {
      formatted.append(EscapeHtml(raw_snippet.substr(last_idx, span.start - last_idx)));
    }
    formatted.append("<mark>");
    formatted.append(EscapeHtml(raw_snippet.substr(span.start, span.end - span.start)));
    formatted.append("</mark>");
    last_idx = span.end;
  }
  if (last_idx < raw_snippet.size()) {
    formatted.append(EscapeHtml(raw_snippet.substr(last_idx)));
  }

  std::string prefix = (start_pos > 0) ? "... " : "";
  std::string suffix = (end_pos < full_text.size()) ? " ..." : "";
  return prefix + formatted + suffix;
}

}  // namespace storage
}  // namespace soeasy
