#include "service/index_config.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/format_categories.h"
#include "common/logger.h"
#include "nlohmann/json.hpp"

namespace soeasy {
namespace service {

IndexConfig::IndexConfig() {
  // Default common excluded directory patterns
  excluded_patterns_ = {
      "/.git/",        "\\git\\",
      "/node_modules/","\\node_modules\\",
      "/.idea/",       "\\.idea\\",
      "/.vscode/",     "\\.vscode\\",
      "/build/",       "\\build\\",
      "/dist/",        "\\dist\\",
      "/__pycache__/", "\\__pycache__\\",
      "/.svn/",        "\\.svn\\",
      "/target/",      "\\target\\",
      "/.cache/",      "\\.cache\\",
  };

  supported_exts_ = GetDefaultSupportedExtensions();
}

std::unordered_set<std::string> IndexConfig::GetDefaultSupportedExtensions() {
  return common::GetDefaultSupportedExtensions();
}

void IndexConfig::ResetSupportedExtensionsToDefault() {
  std::lock_guard<std::mutex> lock(mutex_);
  supported_exts_ = GetDefaultSupportedExtensions();
}

void IndexConfig::AddIndexedDirectory(const std::string& dir_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::find(indexed_dirs_.begin(), indexed_dirs_.end(), dir_path) ==
      indexed_dirs_.end()) {
    indexed_dirs_.push_back(dir_path);
  }
}

void IndexConfig::RemoveIndexedDirectory(const std::string& dir_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  indexed_dirs_.erase(
      std::remove(indexed_dirs_.begin(), indexed_dirs_.end(), dir_path),
      indexed_dirs_.end());
}

void IndexConfig::SetIndexedDirectories(
    const std::vector<std::string>& dir_paths) {
  std::lock_guard<std::mutex> lock(mutex_);
  indexed_dirs_ = dir_paths;
}

void IndexConfig::AddExcludedPattern(const std::string& pattern) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::find(excluded_patterns_.begin(), excluded_patterns_.end(), pattern) ==
      excluded_patterns_.end()) {
    excluded_patterns_.push_back(pattern);
  }
}

void IndexConfig::RemoveExcludedPattern(const std::string& pattern) {
  std::lock_guard<std::mutex> lock(mutex_);
  excluded_patterns_.erase(
      std::remove(excluded_patterns_.begin(), excluded_patterns_.end(), pattern),
      excluded_patterns_.end());
}

void IndexConfig::SetExcludedPatterns(
    const std::vector<std::string>& patterns) {
  std::lock_guard<std::mutex> lock(mutex_);
  excluded_patterns_ = patterns;
}

void IndexConfig::AddSupportedExtension(const std::string& ext) {
  std::string lower_ext = ext;
  std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(),
                 ::tolower);
  std::lock_guard<std::mutex> lock(mutex_);
  supported_exts_.insert(lower_ext);
}

void IndexConfig::RemoveSupportedExtension(const std::string& ext) {
  std::string lower_ext = ext;
  std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(),
                 ::tolower);
  std::lock_guard<std::mutex> lock(mutex_);
  supported_exts_.erase(lower_ext);
}

void IndexConfig::SetSupportedExtensions(
    const std::vector<std::string>& extensions) {
  std::lock_guard<std::mutex> lock(mutex_);
  supported_exts_.clear();
  for (const auto& ext : extensions) {
    std::string lower_ext = ext;
    std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(),
                   ::tolower);
    supported_exts_.insert(lower_ext);
  }
}

std::vector<std::string> IndexConfig::GetIndexedDirectories() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return indexed_dirs_;
}

std::vector<std::string> IndexConfig::GetExcludedPatterns() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return excluded_patterns_;
}

std::unordered_set<std::string> IndexConfig::GetSupportedExtensions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return supported_exts_;
}

void IndexConfig::SetCustomExtensions(const std::vector<std::string>& exts) {
  std::lock_guard<std::mutex> lock(mutex_);
  custom_exts_ = exts;
}

std::vector<std::string> IndexConfig::GetCustomExtensions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return custom_exts_;
}

void IndexConfig::AddCustomExtension(const std::string& ext) {
  if (ext.empty()) return;
  std::string lower = ext;
  if (lower.front() == '.') lower = lower.substr(1);
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::find(custom_exts_.begin(), custom_exts_.end(), lower) == custom_exts_.end()) {
    custom_exts_.push_back(lower);
  }
}

void IndexConfig::RemoveCustomExtension(const std::string& ext) {
  if (ext.empty()) return;
  std::string lower = ext;
  if (lower.front() == '.') lower = lower.substr(1);
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  std::lock_guard<std::mutex> lock(mutex_);
  custom_exts_.erase(
      std::remove(custom_exts_.begin(), custom_exts_.end(), lower),
      custom_exts_.end());
}

bool IndexConfig::IsExtensionSupported(const std::string& ext) const {
  if (ext.empty()) {
    return false;
  }
  std::string lower_ext = ext;
  if (lower_ext.front() == '.') {
    lower_ext.erase(0, 1);
  }
  std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);
  std::lock_guard<std::mutex> lock(mutex_);
  return supported_exts_.find(lower_ext) != supported_exts_.end();
}

bool IndexConfig::IsFileSupported(const std::string& filename) const {
  size_t dot_pos = filename.find_last_of('.');
  if (dot_pos == std::string::npos || dot_pos == filename.length() - 1) {
    return false;
  }
  return IsExtensionSupported(filename.substr(dot_pos + 1));
}

bool IndexConfig::IsDirectoryExcluded(const std::string& dir_path) const {
  if (dir_path.empty()) {
    return false;
  }
  std::string normalized = dir_path;
  if (normalized.front() != '/') {
    normalized = "/" + normalized;
  }
  if (normalized.back() != '/') {
    normalized += "/";
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& pattern : excluded_patterns_) {
    if (normalized.find(pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool IndexConfig::ShouldIndex(const std::string& file_path,
                              const std::string& ext,
                              uint64_t file_size) const {
  if (file_size > max_file_size_) {
    return false;
  }

  std::string lower_ext = ext;
  std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(),
                 ::tolower);

  std::lock_guard<std::mutex> lock(mutex_);

  // Check exclusion patterns
  for (const auto& pattern : excluded_patterns_) {
    if (file_path.find(pattern) != std::string::npos) {
      return false;
    }
  }

  // Check file extension
  if (supported_exts_.find(lower_ext) == supported_exts_.end()) {
    return false;
  }

  // Pre-filter cache and thumbnail paths for images to save indexing overhead
  if (filter_cache_thumbnails_ &&
      (lower_ext == "png" || lower_ext == "jpg" || lower_ext == "jpeg" ||
       lower_ext == "bmp" || lower_ext == "webp" || lower_ext == "gif" ||
       lower_ext == "tif" || lower_ext == "tiff" || lower_ext == "svg")) {
    static const char* kPathKeywords[] = {
        "/weappicon/", "/thumb/", "/thumbs/", "/emoji/", "/emojis/",
        "/emoticon/",  "/emoticons/", "/avatar/", "/avatars/"
    };
    std::string lower_path = file_path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
    for (const char* kw : kPathKeywords) {
      if (lower_path.find(kw) != std::string::npos) {
        return false;
      }
    }
    if (lower_path.find("_thumb.") != std::string::npos ||
        lower_path.find("_thumbnail.") != std::string::npos) {
      return false;
    }
  }

  return true;
}

bool IndexConfig::LoadFromFile(const std::string& file_path) {
  std::ifstream ifs(file_path);
  if (!ifs.is_open()) {
    return false;
  }
  try {
    nlohmann::json j;
    ifs >> j;
    std::lock_guard<std::mutex> lock(mutex_);
    config_path_ = file_path;

    if (j.contains("indexed_directories") && j["indexed_directories"].is_array()) {
      indexed_dirs_.clear();
      for (const auto& item : j["indexed_directories"]) {
        if (item.is_string()) {
          indexed_dirs_.push_back(item.get<std::string>());
        }
      }
    }

    if (j.contains("excluded_patterns") && j["excluded_patterns"].is_array()) {
      excluded_patterns_.clear();
      for (const auto& item : j["excluded_patterns"]) {
        if (item.is_string()) {
          excluded_patterns_.push_back(item.get<std::string>());
        }
      }
    }

    if (j.contains("supported_extensions") && j["supported_extensions"].is_array()) {
      supported_exts_.clear();
      for (const auto& item : j["supported_extensions"]) {
        if (item.is_string()) {
          supported_exts_.insert(item.get<std::string>());
        }
      }
    }

    if (j.contains("max_file_size") && j["max_file_size"].is_number()) {
      max_file_size_ = j["max_file_size"].get<uint64_t>();
    }

    if (j.contains("debounce_ms") && j["debounce_ms"].is_number()) {
      debounce_ms_ = j["debounce_ms"].get<uint32_t>();
    }

    if (j.contains("worker_count") && j["worker_count"].is_number()) {
      worker_count_ = j["worker_count"].get<size_t>();
      if (worker_count_ == 0) {
        worker_count_ = kDefaultWorkerCount;
      }
    }

    if (j.contains("min_image_text_length") && j["min_image_text_length"].is_number()) {
      min_image_text_length_ = j["min_image_text_length"].get<uint32_t>();
    }

    if (j.contains("filter_cache_thumbnails") && j["filter_cache_thumbnails"].is_boolean()) {
      filter_cache_thumbnails_ = j["filter_cache_thumbnails"].get<bool>();
    }

    if (j.contains("log_level") && j["log_level"].is_string()) {
      log_level_ = j["log_level"].get<std::string>();
    }

    if (j.contains("log_console") && j["log_console"].is_boolean()) {
      log_console_ = j["log_console"].get<bool>();
    }

    if (j.contains("log_sync") && j["log_sync"].is_boolean()) {
      log_sync_ = j["log_sync"].get<bool>();
    }

    if (j.contains("doctotext_logging") && j["doctotext_logging"].is_boolean()) {
      doctotext_logging_ = j["doctotext_logging"].get<bool>();
    }

    if (j.contains("doctotext_log_path") && j["doctotext_log_path"].is_string()) {
      doctotext_log_path_ = j["doctotext_log_path"].get<std::string>();
    }

    if (j.contains("enable_embedded_image_ocr") && j["enable_embedded_image_ocr"].is_boolean()) {
      enable_embedded_image_ocr_ = j["enable_embedded_image_ocr"].get<bool>();
    }

    if (j.contains("ui_language") && j["ui_language"].is_string()) {
      std::string lang = j["ui_language"].get<std::string>();
      ui_language_ = (lang == "en") ? "en" : "zh";
    }

    if (j.contains("custom_extensions") && j["custom_extensions"].is_array()) {
      custom_exts_.clear();
      for (const auto& item : j["custom_extensions"]) {
        if (item.is_string()) {
          custom_exts_.push_back(item.get<std::string>());
        }
      }
    }

    LOG_INFO("[IndexConfig] Successfully loaded config from '{}' ({} dirs, {} patterns, {} exts)",
             file_path, indexed_dirs_.size(), excluded_patterns_.size(), supported_exts_.size());
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("[IndexConfig] Failed to parse config JSON '{}': {}", file_path, e.what());
    return false;
  }
}

bool IndexConfig::SaveToFile(const std::string& file_path) const {
  try {
    nlohmann::json j;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      j["indexed_directories"] = indexed_dirs_;
      j["excluded_patterns"] = excluded_patterns_;
      std::vector<std::string> sorted_exts(supported_exts_.begin(), supported_exts_.end());
      std::sort(sorted_exts.begin(), sorted_exts.end());
      j["supported_extensions"] = sorted_exts;
      j["max_file_size"] = max_file_size_;
      j["debounce_ms"] = debounce_ms_;
      j["worker_count"] = worker_count_;
      j["min_image_text_length"] = min_image_text_length_;
      j["filter_cache_thumbnails"] = filter_cache_thumbnails_;
      j["log_level"] = log_level_;
      j["log_console"] = log_console_;
      j["log_sync"] = log_sync_;
      j["doctotext_logging"] = doctotext_logging_;
      j["doctotext_log_path"] = doctotext_log_path_;
      j["enable_embedded_image_ocr"] = enable_embedded_image_ocr_;
      j["ui_language"] = ui_language_;
      j["custom_extensions"] = custom_exts_;
    }

    std::string temp_path = file_path + ".tmp";
    std::ofstream ofs(temp_path);
    if (!ofs.is_open()) {
      LOG_ERROR("[IndexConfig] Failed to open temp config '{}' for writing", temp_path);
      return false;
    }

    ofs << j.dump(2) << std::endl;
    ofs.close();

    std::error_code ec;
    std::filesystem::rename(temp_path, file_path, ec);
    if (ec) {
      LOG_ERROR("[IndexConfig] Failed to atomically rename '{}' to '{}': {}",
                temp_path, file_path, ec.message());
      return false;
    }

    LOG_INFO("[IndexConfig] Configuration successfully saved to '{}'", file_path);
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("[IndexConfig] Failed to save config to '{}': {}", file_path, e.what());
    return false;
  }
}

}  // namespace service
}  // namespace soeasy
