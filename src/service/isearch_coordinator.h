// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_ISEARCH_COORDINATOR_H_
#define SOEASY_SRC_SERVICE_ISEARCH_COORDINATOR_H_

#include <cstdint>
#include <string>
#include <vector>

#include "service/name_index.h"
#include "storage/istorage_engine.h"

namespace soeasy {
namespace service {

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

/**
 * @brief Abstract contract for the CQRS query/search coordination mid-tier.
 *
 * Exclusively provides read-only operations (filename fast prefix search,
 * FTS5 full-text search, and multi-source hybrid ranking) with query generation cancellation.
 */
class ISearchCoordinator {
 public:
  virtual ~ISearchCoordinator() = default;

  /**
   * @brief Fast in-memory filename & pinyin search via NameIndex.
   */
  virtual std::vector<NameMatchResult> SearchFilename(const std::string &query, int limit = 50, int offset = 0,
                                                      const std::string &path_prefix = "",
                                                      int *out_total_count = nullptr) const = 0;

  /**
   * @brief Encrypted FTS5 full-text search via IStorageEngine read channel.
   */
  virtual std::vector<storage::SearchResultItem> SearchFulltext(const std::string &query, int limit = 50, int offset = 0,
                                                               const std::string &path_prefix = "",
                                                               int *out_total_count = nullptr) = 0;

  /**
   * @brief Multi-source hybrid search merging filename relevance and FTS5 snippets.
   */
  virtual std::vector<HybridSearchResultItem> SearchHybrid(const std::string &query, int limit = 50, int offset = 0,
                                                          const std::string &path_prefix = "",
                                                          int *out_total_count = nullptr) = 0;

  /**
   * @brief Allocate a new generation token for rapid typing debounce and cancellation.
   */
  virtual uint64_t AllocateQueryToken() = 0;

  /**
   * @brief Check whether a previously issued query token is still current/active.
   */
  [[nodiscard]] virtual bool IsQueryTokenCurrent(uint64_t token) const = 0;

  /**
   * @brief Invalidate all in-flight queries.
   */
  virtual void InvalidateActiveQueries() = 0;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_ISEARCH_COORDINATOR_H_
