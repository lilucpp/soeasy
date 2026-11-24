// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_COMMON_PARSE_STATUS_H_
#define SOEASY_SRC_COMMON_PARSE_STATUS_H_

#include <cstdint>
#include <string>

namespace soeasy {
namespace common {

/**
 * @brief Persistent document parse status stored in files.parse_stat and parse_issues.status.
 *
 * Numeric values preserve backward compatibility with existing databases where
 * successfully indexed rows stored parse_stat = 1 and schema default is 0.
 */
enum class ParseStatus : int {
  kUnknown = 0,
  kOk = 1,
  kEncrypted = 2,
  kTimeout = 3,
  kCrashed = 4,
  kFailed = 5,
  kPartialOcr = 6,
  kEmptyContent = 7,
};

inline bool IsIssueParseStatus(ParseStatus status) {
  return static_cast<int>(status) >= static_cast<int>(ParseStatus::kEncrypted);
}

inline ParseStatus ParseStatusFromInt(int value) {
  switch (value) {
    case 1:
      return ParseStatus::kOk;
    case 2:
      return ParseStatus::kEncrypted;
    case 3:
      return ParseStatus::kTimeout;
    case 4:
      return ParseStatus::kCrashed;
    case 5:
      return ParseStatus::kFailed;
    case 6:
      return ParseStatus::kPartialOcr;
    case 7:
      return ParseStatus::kEmptyContent;
    default:
      return ParseStatus::kUnknown;
  }
}

inline const char *ParseStatusToKey(ParseStatus status) {
  switch (status) {
    case ParseStatus::kOk:
      return "ok";
    case ParseStatus::kEncrypted:
      return "encrypted";
    case ParseStatus::kTimeout:
      return "timeout";
    case ParseStatus::kCrashed:
      return "crashed";
    case ParseStatus::kFailed:
      return "failed";
    case ParseStatus::kPartialOcr:
      return "partial_ocr";
    case ParseStatus::kEmptyContent:
      return "empty_content";
    default:
      return "unknown";
  }
}

struct ParseStatusSummary {
  uint64_t total_files = 0;
  uint64_t ok_count = 0;
  uint64_t encrypted_count = 0;
  uint64_t timeout_count = 0;
  uint64_t crashed_count = 0;
  uint64_t failed_count = 0;
  uint64_t partial_ocr_count = 0;
  uint64_t empty_content_count = 0;

  [[nodiscard]] uint64_t failure_count() const {
    return timeout_count + crashed_count + failed_count;
  }

  [[nodiscard]] uint64_t issue_count() const {
    return encrypted_count + partial_ocr_count + empty_content_count + timeout_count + crashed_count + failed_count;
  }
};

struct ParseIssueRecord {
  int64_t doc_id = 0;
  std::string file_name;
  std::string file_path;
  std::string ext;
  uint64_t file_size = 0;
  int64_t mtime = 0;
  ParseStatus status = ParseStatus::kFailed;
  std::string reason;
  int64_t updated_at = 0;
};

}  // namespace common
}  // namespace soeasy

#endif  // SOEASY_SRC_COMMON_PARSE_STATUS_H_
