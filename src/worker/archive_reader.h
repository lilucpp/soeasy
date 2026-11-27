// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_ARCHIVE_READER_H_
#define SOEASY_SRC_WORKER_ARCHIVE_READER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

namespace soeasy {
namespace worker {

enum class ArchiveItemEvent {
  kData = 0,
  kEncrypted = 1,
  kSkippedTooLarge = 2,
  kFailed = 3,
  kHeartbeat = 4,
};

struct ArchiveItem {
  std::string virtual_path;  // Logical path e.g. "/xx/1.zip/sub/1.doc"
  std::string file_name;     // Basename e.g. "1.doc"
  std::string ext;           // Lowercase extension without dot
  uint64_t entry_size = 0;
  int64_t entry_mtime = 0;
  ArchiveItemEvent event = ArchiveItemEvent::kData;
  const char *raw_data = nullptr;
  size_t raw_size = 0;
  std::string message;
};

struct ArchiveExtractOptions {
  std::unordered_set<std::string> enabled_exts;  // Enabled non-archive formats
  uint64_t max_entry_bytes = 50ULL * 1024ULL * 1024ULL;
  size_t max_nested_archive_bytes = 256ULL * 1024ULL * 1024ULL;
  int max_depth = 6;
  size_t max_entries = 100000;
};

// Return true to continue extraction, false to cancel traversal cleanly.
using ArchiveItemCallback = std::function<bool(const ArchiveItem &)>;

/**
 * @brief Streaming, zero-disk-temp archive unpacker backed by libarchive.
 *
 * Traverses single and multi-layer nested archives completely in memory, skipping
 * unenabled formats before decompression and emitting matching leaf items via callback.
 */
class ArchiveReader {
 public:
  ArchiveReader() = default;
  ~ArchiveReader() = default;

  bool Extract(const std::string &archive_path, const ArchiveExtractOptions &options, const ArchiveItemCallback &cb,
               std::string *out_error = nullptr) const;
};

}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_ARCHIVE_READER_H_
