// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "service/search_coordinator.h"

#include <algorithm>
#include <unordered_map>

#include "common/logger.h"

namespace soeasy {
namespace service {

SearchCoordinator::SearchCoordinator(std::shared_ptr<NameIndex> name_index,
                                     std::shared_ptr<storage::IStorageEngine> storage_engine)
    : name_index_(std::move(name_index)), storage_engine_(std::move(storage_engine)) {}

std::vector<NameMatchResult> SearchCoordinator::SearchFilename(const std::string &query, int limit, int offset,
                                                               const std::string &path_prefix,
                                                               int *out_total_count) const {
  if (!name_index_) {
    if (out_total_count) *out_total_count = 0;
    return {};
  }
  return name_index_->Search(query, limit, offset, path_prefix, out_total_count);
}

std::vector<storage::SearchResultItem> SearchCoordinator::SearchFulltext(const std::string &query, int limit,
                                                                         int offset, const std::string &path_prefix,
                                                                         int *out_total_count) {
  if (!storage_engine_) {
    if (out_total_count) *out_total_count = 0;
    return {};
  }
  return storage_engine_->Search(query, limit, offset, path_prefix, out_total_count);
}

std::vector<HybridSearchResultItem> SearchCoordinator::SearchHybrid(const std::string &query, int limit, int offset,
                                                                    const std::string &path_prefix,
                                                                    int *out_total_count) {
  std::vector<HybridSearchResultItem> merged_results;
  std::unordered_map<int64_t, size_t> doc_map;

  // 1. Query filename matches from in-memory NameIndex (boosted)
  if (name_index_) {
    int name_total = 0;
    // Fetch a slightly broader window for ranking merge
    auto name_hits = name_index_->Search(query, std::max(limit * 2, 50), 0, path_prefix, &name_total);
    for (const auto &nh : name_hits) {
      HybridSearchResultItem item;
      item.doc_id = nh.doc_id;
      item.file_name = nh.file_name;
      item.file_path = nh.file_path;
      item.ext = nh.ext;
      item.file_size = nh.file_size;
      item.mtime = nh.mtime;
      item.score = nh.score + 50.0;  // Filename exact/pinyin match bonus
      item.is_filename_match = true;
      item.is_fulltext_match = false;

      doc_map[item.doc_id] = merged_results.size();
      merged_results.push_back(std::move(item));
    }
  }

  // 2. Query fulltext matches from StorageEngine
  if (storage_engine_) {
    int ft_total = 0;
    auto ft_hits = storage_engine_->Search(query, std::max(limit * 2, 50), 0, path_prefix, &ft_total);
    for (const auto &fh : ft_hits) {
      auto it = doc_map.find(fh.doc_id);
      if (it != doc_map.end()) {
        // Document matched both filename and content
        auto &existing = merged_results[it->second];
        existing.is_fulltext_match = true;
        existing.snippet = fh.snippet;
        existing.score += std::abs(fh.rank) * 10.0 + 20.0;  // Double match bonus
      } else {
        // Content-only match
        HybridSearchResultItem item;
        item.doc_id = fh.doc_id;
        item.file_name = fh.file_name;
        item.file_path = fh.file_path;
        item.ext = fh.ext;
        item.file_size = fh.file_size;
        item.mtime = fh.mtime;
        item.score = std::abs(fh.rank) * 10.0;
        item.snippet = fh.snippet;
        item.is_filename_match = false;
        item.is_fulltext_match = true;

        doc_map[item.doc_id] = merged_results.size();
        merged_results.push_back(std::move(item));
      }
    }
  }

  // 3. Sort by aggregated score descending
  std::sort(merged_results.begin(), merged_results.end(),
            [](const HybridSearchResultItem &a, const HybridSearchResultItem &b) {
              if (a.score != b.score) return a.score > b.score;
              return a.mtime > b.mtime;
            });

  if (out_total_count) {
    *out_total_count = static_cast<int>(merged_results.size());
  }

  // 4. Apply pagination slice
  if (offset >= static_cast<int>(merged_results.size())) {
    return {};
  }
  int end_idx = std::min(offset + limit, static_cast<int>(merged_results.size()));
  return std::vector<HybridSearchResultItem>(merged_results.begin() + offset, merged_results.begin() + end_idx);
}

uint64_t SearchCoordinator::AllocateQueryToken() {
  return current_query_token_.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool SearchCoordinator::IsQueryTokenCurrent(uint64_t token) const {
  return token == current_query_token_.load(std::memory_order_relaxed);
}

void SearchCoordinator::InvalidateActiveQueries() {
  current_query_token_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace service
}  // namespace soeasy
