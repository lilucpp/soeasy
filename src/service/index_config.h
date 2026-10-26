#ifndef SOEASY_SRC_SERVICE_INDEX_CONFIG_H_
#define SOEASY_SRC_SERVICE_INDEX_CONFIG_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace soeasy {
namespace service {

class IndexConfig {
 public:
  static constexpr uint64_t kDefaultMaxFileSize = 50 * 1024 * 1024;  // 50MB
  static constexpr uint32_t kDefaultDebounceMs = 300;                // 300ms

  IndexConfig();
  ~IndexConfig() = default;

  // Configuration modifiers
  void AddIndexedDirectory(const std::string& dir_path);
  void RemoveIndexedDirectory(const std::string& dir_path);
  void SetIndexedDirectories(const std::vector<std::string>& dir_paths);

  void AddExcludedPattern(const std::string& pattern);
  void RemoveExcludedPattern(const std::string& pattern);
  void SetExcludedPatterns(const std::vector<std::string>& patterns);

  void AddSupportedExtension(const std::string& ext);
  void RemoveSupportedExtension(const std::string& ext);
  void SetSupportedExtensions(const std::vector<std::string>& extensions);

  void set_max_file_size(uint64_t bytes) { max_file_size_ = bytes; }
  uint64_t max_file_size() const { return max_file_size_; }

  void set_debounce_ms(uint32_t ms) { debounce_ms_ = ms; }
  uint32_t debounce_ms() const { return debounce_ms_; }

  std::vector<std::string> GetIndexedDirectories() const;
  std::vector<std::string> GetExcludedPatterns() const;
  std::unordered_set<std::string> GetSupportedExtensions() const;

  /**
   * @brief Check whether a specific file should be indexed according to rules.
   *
   * @param file_path Canonical filesystem path
   * @param ext Extension without dot in lowercase
   * @param file_size Size on disk in bytes
   * @return true if file passes all filter criteria
   */
  bool ShouldIndex(const std::string& file_path, const std::string& ext,
                   uint64_t file_size) const;

  /**
   * @brief Check whether a directory path is excluded (e.g. .git, node_modules).
   */
  bool IsDirectoryExcluded(const std::string& dir_path) const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> indexed_dirs_;
  std::vector<std::string> excluded_patterns_;
  std::unordered_set<std::string> supported_exts_;
  uint64_t max_file_size_ = kDefaultMaxFileSize;
  uint32_t debounce_ms_ = kDefaultDebounceMs;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_INDEX_CONFIG_H_
