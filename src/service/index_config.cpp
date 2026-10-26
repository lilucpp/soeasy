#include "service/index_config.h"

#include <cctype>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

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

  // Default supported file extensions (L1 text/code + L2 documents)
  supported_exts_ = {
      "txt", "md", "markdown", "log", "rtf", "ini", "conf", "cfg", "toml",
      "yaml", "yml", "json", "xml", "html", "htm", "css", "sql", "sh", "bat",
      "cpp", "c", "cc", "cxx", "h", "hpp", "hxx", "py", "java", "go", "rs",
      "js", "ts", "jsx", "tsx", "php", "rb", "swift", "kt", "scala",
      "docx", "pdf", "pptx", "xlsx", "doc", "ppt", "xls"
  };
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

bool IndexConfig::IsDirectoryExcluded(const std::string& dir_path) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& pattern : excluded_patterns_) {
    if (dir_path.find(pattern) != std::string::npos) {
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
  return supported_exts_.find(lower_ext) != supported_exts_.end();
}

}  // namespace service
}  // namespace soeasy
