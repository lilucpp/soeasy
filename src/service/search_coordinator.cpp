// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "service/search_coordinator.h"

#include <algorithm>
#include <unordered_map>

#include "common/logger.h"
#include "common/snippet_highlighter.h"

namespace soeasy {
namespace service {

namespace {

constexpr int kMaxQueryLimit = 1000;
constexpr size_t kMaxBodySnippetsPerPage = 10;
constexpr size_t kBodySnippetMaxChars = 140;
constexpr size_t kNameSnippetMaxChars = 100;

std::string ExtractBodySnippet(storage::IStorageEngine *storage, int64_t doc_id,
                               const std::vector<std::string> &keywords, size_t max_len) {
  if (!storage || keywords.empty()) {
    return {};
  }
  std::string full_text;
  if (!storage->GetDocumentContent(doc_id, &full_text) || full_text.empty()) {
    return {};
  }
  return common::SnippetHighlighter::GenerateSnippet(full_text, keywords, max_len);
}

void PopulateSnippetFromStorage(storage::IStorageEngine *storage, const std::vector<std::string> &keywords,
                                int64_t doc_id, const std::string &file_name, bool allow_body_fetch,
                                std::string *out_snippet, bool *out_has_body) {
  if (!out_snippet) {
    return;
  }
  if (out_has_body) {
    *out_has_body = false;
  }
  if (allow_body_fetch) {
    std::string body_snippet = ExtractBodySnippet(storage, doc_id, keywords, kBodySnippetMaxChars);
    if (!body_snippet.empty()) {
      *out_snippet = std::move(body_snippet);
      if (out_has_body) {
        *out_has_body = true;
      }
      return;
    }
  }
  if (out_snippet->empty()) {
    *out_snippet = common::SnippetHighlighter::GenerateSnippet(file_name, keywords, kNameSnippetMaxChars);
  }
}

struct PagingWindow {
  int limit = 0;
  int offset = 0;
  bool valid = false;
};

/**
 * @brief Sanitize caller-supplied pagination so negative/overflowing values can never reach
 *        iterator arithmetic or SQL LIMIT/OFFSET.
 */
PagingWindow NormalizePaging(PagingWindow req) {
  if (req.offset < 0) {
    req.offset = 0;
  }
  if (req.limit <= 0) {
    req.limit = 0;
    req.valid = false;
    return req;
  }
  if (req.limit > kMaxQueryLimit) {
    req.limit = kMaxQueryLimit;
  }
  req.valid = true;
  return req;
}

}  // namespace

SearchCoordinator::SearchCoordinator(std::shared_ptr<NameIndex> name_index,
                                     std::shared_ptr<storage::IStorageEngine> storage_engine)
    : name_index_(std::move(name_index)), storage_engine_(std::move(storage_engine)) {}

std::vector<NameMatchResult> SearchCoordinator::DoSearchFilename(const std::string &query, int limit, int offset,
                                                                 const std::string &path_prefix,
                                                                 int *out_total_count) const {
  const PagingWindow page = NormalizePaging({limit, offset, false});
  if (!name_index_) {
    if (out_total_count) *out_total_count = 0;
    return {};
  }
  auto res = name_index_->Search(query, page.limit, page.offset, path_prefix, out_total_count);
  return page.valid ? res : std::vector<NameMatchResult>{};
}

std::vector<SearchResultItem> SearchCoordinator::DoSearchFulltext(const std::string &query, int limit, int offset,
                                                                  const std::string &path_prefix,
                                                                  int *out_total_count) {
  const PagingWindow page = NormalizePaging({limit, offset, false});
  if (!page.valid || !storage_engine_) {
    if (out_total_count) *out_total_count = 0;
    return {};
  }
  auto raw_hits = storage_engine_->Search(query, page.limit, page.offset, path_prefix, out_total_count);
  std::vector<SearchResultItem> results;
  results.reserve(raw_hits.size());
  const auto keywords = common::SnippetHighlighter::ExtractKeywordsFromQuery(query);
  for (size_t i = 0; i < raw_hits.size(); ++i) {
    const auto &raw = raw_hits[i];
    SearchResultItem item;
    item.doc_id = raw.doc_id;
    item.file_name = raw.file_name;
    item.file_path = raw.file_path;
    item.ext = raw.ext;
    item.file_size = raw.file_size;
    item.mtime = raw.mtime;
    item.rank = raw.rank;
    PopulateSnippetFromStorage(storage_engine_.get(), keywords, item.doc_id, item.file_name,
                               i < kMaxBodySnippetsPerPage, &item.snippet, &item.has_body_snippet);
    results.push_back(std::move(item));
  }
  return results;
}

std::vector<HybridSearchResultItem> SearchCoordinator::DoSearchHybrid(const std::string &query, int limit, int offset,
                                                                      const std::string &path_prefix,
                                                                      int *out_total_count) {
  const PagingWindow page_req = NormalizePaging({limit, offset, false});
  if (!page_req.valid) {
    if (out_total_count) *out_total_count = 0;
    return {};
  }
  limit = page_req.limit;
  offset = page_req.offset;
  std::vector<HybridSearchResultItem> merged_results;
  std::unordered_map<int64_t, size_t> doc_map;

  const int fetch_limit = static_cast<int>(std::clamp<int64_t>(
      std::max<int64_t>(static_cast<int64_t>(offset) + static_cast<int64_t>(limit) * 2, 50), 1, 10000));

  // 1. Query filename matches from in-memory NameIndex (boosted)
  if (name_index_) {
    int name_total = 0;
    // Fetch a broad window covering offset + ranking margin
    auto name_hits = name_index_->Search(query, fetch_limit, 0, path_prefix, &name_total);
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
    auto ft_hits = storage_engine_->Search(query, fetch_limit, 0, path_prefix, &ft_total);
    for (const auto &fh : ft_hits) {
      auto it = doc_map.find(fh.doc_id);
      if (it != doc_map.end()) {
        // Document matched both filename and content
        auto &existing = merged_results[it->second];
        existing.is_fulltext_match = true;
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

  // 4. Apply pagination slice and populate snippets only for returned page entries
  const size_t safe_offset = static_cast<size_t>(offset);
  if (safe_offset >= merged_results.size()) {
    return {};
  }
  const size_t end_idx = std::min(merged_results.size(), safe_offset + static_cast<size_t>(limit));
  std::vector<HybridSearchResultItem> page(merged_results.begin() + static_cast<ptrdiff_t>(safe_offset),
                                           merged_results.begin() + static_cast<ptrdiff_t>(end_idx));
  const auto keywords = common::SnippetHighlighter::ExtractKeywordsFromQuery(query);
  for (size_t i = 0; i < page.size(); ++i) {
    auto &item = page[i];
    if (item.is_fulltext_match) {
      PopulateSnippetFromStorage(storage_engine_.get(), keywords, item.doc_id, item.file_name,
                                 i < kMaxBodySnippetsPerPage, &item.snippet, nullptr);
    }
  }
  return page;
}

bool SearchCoordinator::DoGetDocumentContent(int64_t doc_id, std::string *out_content) const {
  if (!storage_engine_ || !out_content) {
    return false;
  }
  return storage_engine_->GetDocumentContent(doc_id, out_content);
}

std::vector<DocumentSnippetItem> SearchCoordinator::DoGenerateDocumentSnippets(const std::vector<int64_t> &doc_ids,
                                                                               const std::string &query,
                                                                               size_t max_len) const {
  if (!storage_engine_ || doc_ids.empty()) {
    return {};
  }
  const auto keywords = common::SnippetHighlighter::ExtractKeywordsFromQuery(query);
  if (keywords.empty()) {
    return {};
  }
  std::vector<DocumentSnippetItem> results;
  results.reserve(doc_ids.size());
  for (int64_t doc_id : doc_ids) {
    std::string snippet = ExtractBodySnippet(storage_engine_.get(), doc_id, keywords, max_len);
    if (!snippet.empty()) {
      results.push_back(DocumentSnippetItem{doc_id, std::move(snippet)});
    }
  }
  return results;
}

uint64_t SearchCoordinator::DoAllocateQueryToken() {
  return current_query_token_.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool SearchCoordinator::DoIsQueryTokenCurrent(uint64_t token) const {
  return token == current_query_token_.load(std::memory_order_relaxed);
}

void SearchCoordinator::DoInvalidateActiveQueries() {
  current_query_token_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace service
}  // namespace soeasy
