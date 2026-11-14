// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_ISEARCH_COORDINATOR_H_
#define SOEASY_SRC_SERVICE_ISEARCH_COORDINATOR_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "service/name_index.h"

namespace soeasy {
namespace service {

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

struct HybridSearchResultItem {
  int64_t doc_id = 0;
  std::string file_name;
  std::string file_path;
  std::string ext;
  uint64_t file_size = 0;
  int64_t mtime = 0;
  double score = 0.0;
  std::string snippet;
  bool is_filename_match = false;
  bool is_fulltext_match = false;
};

struct DocumentSnippetItem {
  int64_t doc_id = 0;
  std::string snippet;
};

/**
 * @brief Abstract contract for the CQRS query/search coordination mid-tier.
 *
 * Exclusively provides read-only operations (filename fast prefix search,
 * FTS5 full-text search, and multi-source hybrid ranking) with query generation cancellation.
 */
class ISearchCoordinator {
 public:
  ISearchCoordinator() = default;
  virtual ~ISearchCoordinator() = default;

  ISearchCoordinator(const ISearchCoordinator &) = delete;
  ISearchCoordinator &operator=(const ISearchCoordinator &) = delete;
  ISearchCoordinator(ISearchCoordinator &&) = delete;
  ISearchCoordinator &operator=(ISearchCoordinator &&) = delete;

  /**
   * @brief Fast in-memory filename & pinyin search via NameIndex (NVI public contract).
   */
  std::vector<NameMatchResult> SearchFilename(const std::string &query, int limit = 50, int offset = 0,
                                              const std::string &path_prefix = "",
                                              int *out_total_count = nullptr) const {
    return DoSearchFilename(query, limit, offset, path_prefix, out_total_count);
  }

  /**
   * @brief Encrypted FTS5 full-text search via IStorageEngine read channel (NVI public contract).
   */
  std::vector<SearchResultItem> SearchFulltext(const std::string &query, int limit = 50, int offset = 0,
                                               const std::string &path_prefix = "", int *out_total_count = nullptr) {
    return DoSearchFulltext(query, limit, offset, path_prefix, out_total_count);
  }

  /**
   * @brief Multi-source hybrid search merging filename relevance and FTS5 snippets (NVI public contract).
   */
  std::vector<HybridSearchResultItem> SearchHybrid(const std::string &query, int limit = 50, int offset = 0,
                                                   const std::string &path_prefix = "",
                                                   int *out_total_count = nullptr) {
    return DoSearchHybrid(query, limit, offset, path_prefix, out_total_count);
  }

  /**
   * @brief Decompress and fetch original UTF-8 document content via read channel.
   */
  bool GetDocumentContent(int64_t doc_id, std::string *out_content) const {
    return DoGetDocumentContent(doc_id, out_content);
  }

  /**
   * @brief Batch-extract highlighted body snippets for the requested documents via read channel.
   */
  [[nodiscard]] std::vector<DocumentSnippetItem> GenerateDocumentSnippets(const std::vector<int64_t> &doc_ids,
                                                                          const std::string &query,
                                                                          size_t max_len = 140) const {
    return DoGenerateDocumentSnippets(doc_ids, query, max_len);
  }

  /**
   * @brief Allocate a new generation token for rapid typing debounce and cancellation.
   */
  uint64_t AllocateQueryToken() { return DoAllocateQueryToken(); }

  /**
   * @brief Check whether a previously issued query token is still current/active.
   */
  [[nodiscard]] bool IsQueryTokenCurrent(uint64_t token) const { return DoIsQueryTokenCurrent(token); }

  /**
   * @brief Invalidate all in-flight queries.
   */
  void InvalidateActiveQueries() { DoInvalidateActiveQueries(); }

 protected:
  virtual std::vector<NameMatchResult> DoSearchFilename(const std::string &query, int limit, int offset,
                                                        const std::string &path_prefix, int *out_total_count) const = 0;

  virtual std::vector<SearchResultItem> DoSearchFulltext(const std::string &query, int limit, int offset,
                                                         const std::string &path_prefix, int *out_total_count) = 0;

  virtual std::vector<HybridSearchResultItem> DoSearchHybrid(const std::string &query, int limit, int offset,
                                                             const std::string &path_prefix, int *out_total_count) = 0;

  virtual bool DoGetDocumentContent(int64_t doc_id, std::string *out_content) const = 0;
  [[nodiscard]] virtual std::vector<DocumentSnippetItem> DoGenerateDocumentSnippets(const std::vector<int64_t> &doc_ids,
                                                                                    const std::string &query,
                                                                                    size_t max_len) const = 0;
  virtual uint64_t DoAllocateQueryToken() = 0;
  [[nodiscard]] virtual bool DoIsQueryTokenCurrent(uint64_t token) const = 0;
  virtual void DoInvalidateActiveQueries() = 0;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_ISEARCH_COORDINATOR_H_
