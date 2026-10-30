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
  static constexpr size_t kDefaultWorkerCount = 2;                   // 2 workers default
  static constexpr const char* kDefaultLogLevel = "info";
  static constexpr bool kDefaultLogConsole = false;
  static constexpr bool kDefaultLogSync = false;

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
  void ResetSupportedExtensionsToDefault();
  static std::unordered_set<std::string> GetDefaultSupportedExtensions();

  void set_max_file_size(uint64_t bytes) { max_file_size_ = bytes; }
  uint64_t max_file_size() const { return max_file_size_; }

  void set_debounce_ms(uint32_t ms) { debounce_ms_ = ms; }
  uint32_t debounce_ms() const { return debounce_ms_; }

  void set_worker_count(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    worker_count_ = count > 0 ? count : kDefaultWorkerCount;
  }
  size_t worker_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return worker_count_;
  }

  void set_log_level(const std::string& level) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_level_ = level;
  }
  std::string log_level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_level_;
  }

  void set_log_console(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_console_ = enable;
  }
  bool log_console() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_console_;
  }

  void set_log_sync(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_sync_ = enable;
  }
  bool log_sync() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_sync_;
  }

  void set_doctotext_logging(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    doctotext_logging_ = enable;
  }
  bool doctotext_logging() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return doctotext_logging_;
  }

  void set_doctotext_log_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    doctotext_log_path_ = path;
  }
  std::string doctotext_log_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return doctotext_log_path_;
  }

  void set_enable_embedded_image_ocr(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    enable_embedded_image_ocr_ = enable;
  }
  bool enable_embedded_image_ocr() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enable_embedded_image_ocr_;
  }

  void SetCustomExtensions(const std::vector<std::string>& exts);
  std::vector<std::string> GetCustomExtensions() const;
  void AddCustomExtension(const std::string& ext);
  void RemoveCustomExtension(const std::string& ext);

  std::vector<std::string> GetIndexedDirectories() const;
  std::vector<std::string> GetExcludedPatterns() const;
  std::unordered_set<std::string> GetSupportedExtensions() const;

  /**
   * @brief Check whether a specific extension is supported.
   *
   * @param ext Extension with or without leading dot
   */
  bool IsExtensionSupported(const std::string& ext) const;

  /**
   * @brief Check whether a filename has a supported extension.
   *
   * @param filename Filename or relative path
   */
  bool IsFileSupported(const std::string& filename) const;

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

  /**
   * @brief Load configuration from a JSON file on disk.
   */
  bool LoadFromFile(const std::string& file_path);

  /**
   * @brief Persist current configuration to a JSON file on disk.
   */
  bool SaveToFile(const std::string& file_path) const;

  void set_min_image_text_length(uint32_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_image_text_length_ = len;
  }
  uint32_t min_image_text_length() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return min_image_text_length_;
  }

  void set_filter_cache_thumbnails(bool filter) {
    std::lock_guard<std::mutex> lock(mutex_);
    filter_cache_thumbnails_ = filter;
  }
  bool filter_cache_thumbnails() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return filter_cache_thumbnails_;
  }

  void set_config_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_path_ = path;
  }
  std::string config_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_path_;
  }
  bool Save() const {
    std::string path;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      path = config_path_;
    }
    if (path.empty()) return false;
    return SaveToFile(path);
  }

 private:
  mutable std::mutex mutex_;
  std::string config_path_;
  std::vector<std::string> indexed_dirs_;
  std::vector<std::string> excluded_patterns_;
  std::unordered_set<std::string> supported_exts_;
  uint64_t max_file_size_ = kDefaultMaxFileSize;
  uint32_t debounce_ms_ = kDefaultDebounceMs;
  size_t worker_count_ = kDefaultWorkerCount;
  uint32_t min_image_text_length_ = 4;
  bool filter_cache_thumbnails_ = true;
  std::string log_level_ = kDefaultLogLevel;
  bool log_console_ = kDefaultLogConsole;
  bool log_sync_ = kDefaultLogSync;
  bool doctotext_logging_ = false;
  std::string doctotext_log_path_;
  bool enable_embedded_image_ocr_ = false;
  std::vector<std::string> custom_exts_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_INDEX_CONFIG_H_
