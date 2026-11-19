// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_STORAGE_ISTORAGE_WRITER_H_
#define SOEASY_SRC_STORAGE_ISTORAGE_WRITER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace soeasy {
namespace storage {

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
 * @brief Narrow role interface for document indexing and write operations (ISP / CQRS write channel).
 */
class IDocumentIndexWriter {
 public:
  virtual ~IDocumentIndexWriter() = default;

  virtual bool BeginTransaction() = 0;
  virtual bool CommitTransaction() = 0;
  virtual bool RollbackTransaction() = 0;
  [[nodiscard]] virtual bool IsInTransaction() const = 0;
  [[nodiscard]] virtual size_t GetUncommittedCount() const = 0;

  virtual bool IndexDocument(const DocumentIndexEntry &entry) = 0;
  virtual bool DeleteDocument(int64_t doc_id) = 0;
  virtual bool DeleteDocumentsBatch(const std::vector<int64_t> &doc_ids) = 0;
  virtual int64_t DeleteDirectory(const std::string &dir_path, std::vector<int64_t> *out_deleted_doc_ids = nullptr) = 0;
  virtual bool ClearAll() = 0;
  virtual bool SetMetaValue(const std::string &key, const std::string &value) = 0;
};

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_ISTORAGE_WRITER_H_
