// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_FILE_CRAWLER_H_
#define SOEASY_SRC_SERVICE_FILE_CRAWLER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "service/index_config.h"

namespace soeasy {
namespace service {

struct ScanCandidatesResult {
  std::vector<std::string> files_to_index;
  std::unordered_set<int64_t> visited_doc_ids;
  std::vector<std::string> traversed_root_prefixes;
  bool was_cancelled = false;
};

/**
 * @brief Standalone filesystem crawler module.
 *
 * Recursively discovers candidate files across directories with bind-mount deduplication,
 * cancellation responsiveness, and path exclusion filtering.
 */
class FileCrawler {
 public:
  static ScanCandidatesResult CollectCandidates(const std::vector<std::string> &dirs, const IndexConfig &config,
                                                const std::atomic<bool> &cancel_signal);

  static void WalkDirectory(
      const std::string &dir_path, const IndexConfig &config, const std::atomic<bool> &cancel_signal,
      std::function<void(const std::string &file_path, uint64_t file_size, int64_t mtime)> on_file);

  static int64_t GenerateDocId(const std::string &path);
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_FILE_CRAWLER_H_
