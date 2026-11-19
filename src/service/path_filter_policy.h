// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_PATH_FILTER_POLICY_H_
#define SOEASY_SRC_SERVICE_PATH_FILTER_POLICY_H_

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace soeasy {
namespace service {

/**
 * @brief Pure policy engine for path and extension filtering.
 *
 * Decouples filesystem rule matching, Linux VFS superblock detection,
 * Windows Reparse Point junction filtering, and thumbnail heuristics from configuration state.
 */
class PathFilterPolicy {
 public:
  /**
   * @brief Check whether a directory path is excluded from scanning/watching.
   */
  static bool IsDirectoryExcluded(const std::string &dir_path, const std::vector<std::string> &excluded_patterns);

  /**
   * @brief Check whether a file passes all index inclusion criteria.
   */
  static bool ShouldIndex(const std::string &file_path, const std::string &ext, uint64_t file_size,
                          uint64_t max_file_size, const std::vector<std::string> &excluded_patterns,
                          const std::unordered_set<std::string> &supported_exts, bool filter_cache_thumbnails = true);

  /**
   * @brief Normalize path separators to forward slashes.
   */
  static std::string NormalizePath(const std::string &path);
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_PATH_FILTER_POLICY_H_
