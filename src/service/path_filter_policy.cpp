// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "service/path_filter_policy.h"

#include <algorithm>

#include "common/format_categories.h"

#ifdef _WIN32
#include <windows.h>

#include "common/encoding_utils.h"
#else
#include <sys/vfs.h>
#endif

namespace soeasy {
namespace service {

std::string PathFilterPolicy::NormalizePath(const std::string &path) {
  std::string norm = path;
  std::replace(norm.begin(), norm.end(), '\\', '/');
  return norm;
}

bool PathFilterPolicy::IsDirectoryExcluded(const std::string &dir_path,
                                           const std::vector<std::string> &excluded_patterns) {
  if (dir_path.empty()) {
    return false;
  }

#ifndef _WIN32
  // 1. Linux statfs.f_type: probe virtual / pseudo / RAM filesystems (< 50ns)
  struct statfs fs_info;
  if (statfs(dir_path.c_str(), &fs_info) == 0) {
    switch (fs_info.f_type) {
      case 0x9fa0:      // PROC_SUPER_MAGIC (/proc)
      case 0x62656572:  // SYSFS_MAGIC_NUMBER (/sys)
      case 0x01021994:  // TMPFS_MAGIC (/run, /dev/shm, etc.)
      case 0x858458f6:  // RAMFS_MAGIC
      case 0x1cd1:      // DEVPTS_SUPER_MAGIC (/dev/pts)
      case 0x27e0eb:    // CGROUP_SUPER_MAGIC
      case 0x63677270:  // CGROUP2_SUPER_MAGIC
      case 0x42494e4d:  // BINFMTFS_MAGIC
      case 0x534f434b:  // SOCKFS_MAGIC
        return true;
    }
  }
#else
  // 2. Windows NTFS Reparse Points (Junction loop prevention) & System Volume
  std::wstring wpath = common::EncodingUtils::Utf8ToWide(dir_path);
  DWORD attrs = GetFileAttributesW(wpath.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES) {
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
      return true;  // Stop Junction loops (e.g. Application Data infinite recursion)
    }
    if ((attrs & FILE_ATTRIBUTE_SYSTEM) && (attrs & FILE_ATTRIBUTE_HIDDEN)) {
      return true;  // Stop System Volume Information & hidden system caches
    }
  }
#endif

  // 3. Normalized pattern matching
  std::string normalized = NormalizePath(dir_path);
  if (normalized.front() != '/') {
    normalized = "/" + normalized;
  }
  if (normalized.back() != '/') {
    normalized += "/";
  }

#ifndef _WIN32
  // Skip root-level ephemeral / recovery folders when traversing from root
  if (normalized == "/tmp/" || normalized == "/lost+found/") {
    return true;
  }
#endif

  for (const auto &pattern : excluded_patterns) {
    if (pattern.empty()) {
      continue;
    }
    std::string norm_pattern = NormalizePath(pattern);
    if (normalized.find(norm_pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool PathFilterPolicy::ShouldIndex(const std::string &file_path, const std::string &ext, uint64_t file_size,
                                   uint64_t max_file_size, const std::vector<std::string> &excluded_patterns,
                                   const std::unordered_set<std::string> &supported_exts,
                                   bool filter_cache_thumbnails) {
  std::string lower_ext = ext;
  if (!lower_ext.empty() && lower_ext.front() == '.') {
    lower_ext.erase(0, 1);
  }
  std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);

  // Host archives have no whole-archive size ceiling (size limit applies only per extracted leaf entry).
  if (file_size > max_file_size && !common::IsSupportedArchiveExtension(lower_ext)) {
    return false;
  }

  // Check exclusion patterns
  std::string norm_path = NormalizePath(file_path);
  for (const auto &pattern : excluded_patterns) {
    if (pattern.empty()) {
      continue;
    }
    std::string norm_pattern = NormalizePath(pattern);
    if (norm_path.find(norm_pattern) != std::string::npos) {
      return false;
    }
  }

  // Check file extension
  if (supported_exts.find(lower_ext) == supported_exts.end()) {
    return false;
  }

  // Pre-filter cache and thumbnail paths for images to save indexing overhead
  if (filter_cache_thumbnails && common::IsSupportedImageExtension(lower_ext)) {
    static const char *kPathKeywords[] = {"/weappicon/", "/thumb/",     "/thumbs/", "/emoji/",  "/emojis/",
                                          "/emoticon/",  "/emoticons/", "/avatar/", "/avatars/"};
    std::string lower_path = norm_path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
    for (const char *kw : kPathKeywords) {
      if (lower_path.find(kw) != std::string::npos) {
        return false;
      }
    }
    if (lower_path.find("_thumb.") != std::string::npos || lower_path.find("_thumbnail.") != std::string::npos) {
      return false;
    }
  }

  return true;
}

}  // namespace service
}  // namespace soeasy
