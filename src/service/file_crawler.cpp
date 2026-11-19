// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "service/file_crawler.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "common/encoding_utils.h"
#include "common/logger.h"
#include "service/path_filter_policy.h"

namespace soeasy {
namespace service {

namespace {

struct DirFingerprint {
  uint64_t dev{0};
  uint64_t ino{0};
  bool operator==(const DirFingerprint &other) const { return dev == other.dev && ino == other.ino; }
};

struct DirFingerprintHash {
  std::size_t operator()(const DirFingerprint &fp) const {
    return std::hash<uint64_t>()(fp.dev) ^ (std::hash<uint64_t>()(fp.ino) << 1);
  }
};

bool GetDirFingerprint(const std::filesystem::path &p, DirFingerprint *fp) {
#ifndef _WIN32
  struct stat st;
  if (::stat(p.c_str(), &st) == 0) {
    if (fp) {
      fp->dev = static_cast<uint64_t>(st.st_dev);
      fp->ino = static_cast<uint64_t>(st.st_ino);
    }
    return true;
  }
  return false;
#else
  HANDLE h_file = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h_file != INVALID_HANDLE_VALUE) {
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(h_file, &info)) {
      if (fp) {
        fp->dev = static_cast<uint64_t>(info.dwVolumeSerialNumber);
        fp->ino = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
      }
      CloseHandle(h_file);
      return true;
    }
    CloseHandle(h_file);
  }
  return false;
#endif
}

std::string ExtractExtension(const std::string &filename) {
  size_t dot_pos = filename.find_last_of('.');
  if (dot_pos == std::string::npos || dot_pos == 0) {
    return "";
  }
  std::string ext = filename.substr(dot_pos + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return ext;
}

std::string NormalizeDirPrefixSlash(const std::string &dir_path) {
  const std::string norm = NormalizeDirKey(dir_path);
  return (norm == "/") ? norm : (norm + "/");
}

}  // namespace

int64_t FileCrawler::GenerateDocId(const std::string &path) {
  constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
  constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
  uint64_t hash = kFnvOffsetBasis;
  for (char c : path) {
    hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
    hash *= kFnvPrime;
  }
  return static_cast<int64_t>(hash & 0x7FFFFFFFFFFFFFFFLL);
}

ScanCandidatesResult FileCrawler::CollectCandidates(const std::vector<std::string> &dirs, const IndexConfig &config,
                                                    const std::atomic<bool> &cancel_signal) {
  ScanCandidatesResult result;
  std::unordered_set<DirFingerprint, DirFingerprintHash> visited_dirs;

  for (const auto &dir : dirs) {
    if (cancel_signal.load(std::memory_order_acquire)) {
      result.was_cancelled = true;
      break;
    }
    if (dir.empty() || config.IsDirectoryExcluded(dir)) {
      continue;
    }

    std::filesystem::path root_path = common::EncodingUtils::PathFromUtf8(dir);
    if (!std::filesystem::exists(root_path)) {
      continue;
    }

    DirFingerprint root_fp;
    if (GetDirFingerprint(root_path, &root_fp)) {
      if (!visited_dirs.insert(root_fp).second) {
        LOG_DEBUG("[FileCrawler] Skipping already visited root directory: '{}'", dir);
        continue;
      }
    }

    bool dir_completed = true;
    try {
      auto opts = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::recursive_directory_iterator it(root_path, opts), end; it != end; ++it) {
        if (cancel_signal.load(std::memory_order_acquire)) {
          LOG_INFO("[FileCrawler] Scan cancelled by signal during directory walk.");
          dir_completed = false;
          result.was_cancelled = true;
          break;
        }

        std::error_code ec_stat;
        bool is_dir = it->is_directory(ec_stat);
        if (!ec_stat && is_dir) {
          std::string sub_dir = common::EncodingUtils::PathToUtf8(it->path());
          if (config.IsDirectoryExcluded(sub_dir)) {
            it.disable_recursion_pending();
            continue;
          }
          DirFingerprint fp;
          if (GetDirFingerprint(it->path(), &fp)) {
            if (!visited_dirs.insert(fp).second) {
              LOG_DEBUG("[FileCrawler] Skipping duplicate/bind-mounted directory: '{}'", sub_dir);
              it.disable_recursion_pending();
              continue;
            }
          }
        }

        ec_stat.clear();
        bool is_reg = it->is_regular_file(ec_stat);
        if (!ec_stat && is_reg) {
          std::string fpath = common::EncodingUtils::PathToUtf8(it->path());
          result.visited_doc_ids.insert(GenerateDocId(fpath));
          std::string fname = common::EncodingUtils::PathToUtf8(it->path().filename());
          std::string ext = ExtractExtension(fname);
          uint64_t fsize = 0;
          int64_t fmtime = 0;
          if (!common::EncodingUtils::GetFileStat(fpath, &fsize, &fmtime)) {
            std::error_code ec_size;
            fsize = it->file_size(ec_size);
            if (ec_size) {
              LOG_DEBUG("[FileCrawler] Unable to get file size for '{}': {}", fpath, ec_size.message());
            }
          }

          if (config.ShouldIndex(fpath, ext, fsize)) {
            result.files_to_index.push_back(fpath);
          }
        }
      }
      if (dir_completed) {
        result.traversed_root_prefixes.push_back(NormalizeDirPrefixSlash(dir));
      }
    } catch (const std::exception &e) {
      LOG_WARN("[FileCrawler] Failed to scan watched directory '{}': {}", dir, e.what());
    }
  }

  return result;
}

void FileCrawler::WalkDirectory(
    const std::string &dir_path, const IndexConfig &config, const std::atomic<bool> &cancel_signal,
    std::function<void(const std::string &file_path, uint64_t file_size, int64_t mtime)> on_file) {
  if (dir_path.empty() || config.IsDirectoryExcluded(dir_path)) {
    return;
  }

  std::filesystem::path root = common::EncodingUtils::PathFromUtf8(dir_path);
  if (!std::filesystem::exists(root)) {
    return;
  }

  std::unordered_set<DirFingerprint, DirFingerprintHash> visited_dirs;
  DirFingerprint root_fp;
  if (GetDirFingerprint(root, &root_fp)) {
    visited_dirs.insert(root_fp);
  }

  try {
    auto opts = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, opts), end; it != end; ++it) {
      if (cancel_signal.load(std::memory_order_acquire)) {
        break;
      }

      std::error_code ec_stat;
      bool is_dir = it->is_directory(ec_stat);
      if (!ec_stat && is_dir) {
        std::string sub_dir = common::EncodingUtils::PathToUtf8(it->path());
        if (config.IsDirectoryExcluded(sub_dir)) {
          it.disable_recursion_pending();
          continue;
        }
        DirFingerprint fp;
        if (GetDirFingerprint(it->path(), &fp)) {
          if (!visited_dirs.insert(fp).second) {
            it.disable_recursion_pending();
            continue;
          }
        }
      }

      ec_stat.clear();
      bool is_reg = it->is_regular_file(ec_stat);
      if (!ec_stat && is_reg) {
        std::string fpath = common::EncodingUtils::PathToUtf8(it->path());
        std::string fname = common::EncodingUtils::PathToUtf8(it->path().filename());
        std::string ext = ExtractExtension(fname);
        uint64_t fsize = 0;
        int64_t fmtime = 0;
        if (!common::EncodingUtils::GetFileStat(fpath, &fsize, &fmtime)) {
          std::error_code ec_size;
          fsize = it->file_size(ec_size);
          if (ec_size) {
            LOG_DEBUG("[FileCrawler] Unable to get file size for '{}': {}", fpath, ec_size.message());
          }
        }

        if (config.ShouldIndex(fpath, ext, fsize) && on_file) {
          on_file(fpath, fsize, fmtime);
        }
      }
    }
  } catch (const std::exception &e) {
    LOG_WARN("[FileCrawler] Failed to walk directory '{}': {}", dir_path, e.what());
  }
}

}  // namespace service
}  // namespace soeasy
