// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_SEARCH_COORDINATOR_H_
#define SOEASY_SRC_SERVICE_SEARCH_COORDINATOR_H_

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "service/isearch_coordinator.h"
#include "service/name_index.h"
#include "storage/istorage_engine.h"

namespace soeasy {
namespace service {

/**
 * @brief CQRS Query-side implementation of SearchCoordinator.
 *
 * Coordinates read-only queries across in-memory trie (NameIndex) and SQLite WAL FTS5 (IStorageEngine).
 * Decoupled from write operations, indexing pipelines, and filesystem watcher events.
 */
class SearchCoordinator final : public ISearchCoordinator {
 public:
  SearchCoordinator(std::shared_ptr<NameIndex> name_index, std::shared_ptr<storage::IStorageEngine> storage_engine);
  ~SearchCoordinator() override = default;

 protected:
  std::vector<NameMatchResult> DoSearchFilename(const std::string &query, int limit, int offset,
                                                const std::string &path_prefix, int *out_total_count) const override;

  std::vector<SearchResultItem> DoSearchFulltext(const std::string &query, int limit, int offset,
                                                 const std::string &path_prefix, int *out_total_count) override;

  std::vector<HybridSearchResultItem> DoSearchHybrid(const std::string &query, int limit, int offset,
                                                     const std::string &path_prefix, int *out_total_count) override;

  bool DoGetDocumentContent(int64_t doc_id, std::string *out_content) const override;

  [[nodiscard]] std::vector<DocumentSnippetItem> DoGenerateDocumentSnippets(const std::vector<int64_t> &doc_ids,
                                                                            const std::string &query,
                                                                            size_t max_len) const override;

  uint64_t DoAllocateQueryToken() override;

  [[nodiscard]] bool DoIsQueryTokenCurrent(uint64_t token) const override;

  void DoInvalidateActiveQueries() override;

 private:
  std::shared_ptr<NameIndex> name_index_;
  std::shared_ptr<storage::IStorageEngine> storage_engine_;
  std::atomic<uint64_t> current_query_token_{0};
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_SEARCH_COORDINATOR_H_
