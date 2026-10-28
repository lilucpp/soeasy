#ifndef SOEASY_SRC_SERVICE_NAME_INDEX_H_
#define SOEASY_SRC_SERVICE_NAME_INDEX_H_

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace soeasy {
namespace service {

struct NameRecord {
  int64_t doc_id = 0;
  std::string file_name;
  std::string file_path;
  std::string ext;
  uint64_t file_size = 0;
  int64_t mtime = 0;
};

struct NameMatchResult {
  int64_t doc_id = 0;
  std::string file_name;
  std::string file_path;
  std::string ext;
  uint64_t file_size = 0;
  int64_t mtime = 0;
  double score = 0.0;
};

class NameIndex {
 public:
  NameIndex() = default;
  ~NameIndex() = default;

  // Disallow copy and assign
  NameIndex(const NameIndex&) = delete;
  NameIndex& operator=(const NameIndex&) = delete;

  /**
   * @brief Insert or update a file record in the in-memory filename index.
   */
  void Upsert(const NameRecord& record);

  /**
   * @brief Remove a file record by doc_id.
   */
  void Remove(int64_t doc_id);

  /**
   * @brief Remove a file record by file path.
   */
  void RemoveByPath(const std::string& file_path);

  /**
   * @brief Retrieve metadata (file_size, mtime) for rapid in-memory incremental check.
   */
  bool GetMetadata(int64_t doc_id, uint64_t* out_file_size, int64_t* out_mtime) const;

  /**
   * @brief Instantaneous search by filename query with optional path scope filtering.
   *
   * @param query Search query keyword or pattern
   * @param limit Maximum results to return
   * @param path_prefix Optional path filter: only return hits located under this path
   * @return std::vector<NameMatchResult> Ranked results
   */
  std::vector<NameMatchResult> Search(const std::string& query,
                                      int limit = 50,
                                      const std::string& path_prefix = "") const;

  /**
   * @brief Get total number of indexed files in memory.
   */
  size_t size() const;

  /**
   * @brief Clear all records.
   */
  void Clear();

 private:
  static std::string ToLower(const std::string& s);
  static bool StartsWith(const std::string& str, const std::string& prefix);

  mutable std::shared_mutex rw_mutex_;
  std::unordered_map<int64_t, NameRecord> records_by_id_;
  std::unordered_map<std::string, int64_t> id_by_path_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_NAME_INDEX_H_
