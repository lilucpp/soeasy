// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_STORAGE_ISTORAGE_READER_H_
#define SOEASY_SRC_STORAGE_ISTORAGE_READER_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/parse_status.h"

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

using FileMetaCallback = std::function<void(const FileMetaRecord &)>;

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
 * @brief ISP Read-only role interface for document search, content retrieval and metadata streaming.
 *
 * Adheres to Interface Segregation Principle (ISP) and CQRS, allowing query coordinators
 * (like SearchCoordinator) to depend solely on read capabilities without exposing write mutations.
 */
class IDocumentSearchReader {
 public:
  virtual ~IDocumentSearchReader() = default;

  virtual std::vector<SearchResultItem> Search(const std::string &query, int limit = 50, int offset = 0,
                                               const std::string &path_prefix = "", int *out_total_count = nullptr) = 0;

  virtual bool GetDocumentContent(int64_t doc_id, std::string *out_content) = 0;

  virtual bool GetFileMetadata(int64_t doc_id, uint64_t *out_file_size, int64_t *out_mtime,
                               int *out_parse_stat = nullptr) = 0;

  virtual bool GetDocumentParseInfo(int64_t doc_id, common::ParseStatus *out_status, std::string *out_reason) {
    int parse_stat = 0;
    if (!GetFileMetadata(doc_id, nullptr, nullptr, &parse_stat)) {
      if (out_status) *out_status = common::ParseStatus::kUnknown;
      if (out_reason) out_reason->clear();
      return false;
    }
    if (out_status) *out_status = common::ParseStatusFromInt(parse_stat);
    if (out_reason) out_reason->clear();
    return true;
  }

  virtual std::vector<FileMetaRecord> GetAllFilesMetadata() = 0;

  virtual bool ForEachFileMetadata(FileMetaCallback callback) = 0;

  virtual ScopedReadConnection AcquireReadConnection() = 0;

  virtual bool GetMetaValue(const std::string &key, std::string *out_value) = 0;

  virtual common::ParseStatusSummary GetParseStatusSummary() = 0;

  virtual std::vector<common::ParseIssueRecord> GetParseIssues(
      std::optional<common::ParseStatus> filter_status = std::nullopt, size_t limit = 500, size_t offset = 0) = 0;
};

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_ISTORAGE_READER_H_
