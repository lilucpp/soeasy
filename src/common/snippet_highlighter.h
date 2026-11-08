// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_COMMON_SNIPPET_HIGHLIGHTER_H_
#define SOEASY_SRC_COMMON_SNIPPET_HIGHLIGHTER_H_

#include <cstddef>
#include <string>
#include <vector>

namespace soeasy {
namespace common {

/**
 * @brief Utility class for keyword extraction, sliding window snippet generation,
 * and HTML <mark> tag highlighting. Decoupled from persistent storage engines.
 */
class SnippetHighlighter {
 public:
  static constexpr size_t kMaxScanBytes = 128 * 1024;  // 128KB max text scan window

  /**
   * @brief Parse FTS / query tokens into clean keyword list for highlighting.
   */
  static std::vector<std::string> ExtractKeywordsFromQuery(const std::string &query);

  /**
   * @brief Generate a sliding window snippet around the earliest keyword match with <mark> tags.
   */
  static std::string GenerateSnippet(const std::string &full_text,
                                     const std::vector<std::string> &keywords,
                                     size_t max_chars = 120);
};

}  // namespace common
}  // namespace soeasy

#endif  // SOEASY_SRC_COMMON_SNIPPET_HIGHLIGHTER_H_
