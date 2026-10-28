#include "service/name_index.h"

#include <cctype>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace soeasy {
namespace service {

std::string NameIndex::ToLower(const std::string& s) {
  std::string lower = s;
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return lower;
}

bool NameIndex::StartsWith(const std::string& str, const std::string& prefix) {
  if (prefix.size() > str.size()) {
    return false;
  }
  return std::equal(prefix.begin(), prefix.end(), str.begin());
}

void NameIndex::Upsert(const NameRecord& record) {
  std::unique_lock<std::shared_mutex> lock(rw_mutex_);

  // If path already exists with a different ID, remove old mapping
  auto it_path = id_by_path_.find(record.file_path);
  if (it_path != id_by_path_.end() && it_path->second != record.doc_id) {
    records_by_id_.erase(it_path->second);
  }

  id_by_path_[record.file_path] = record.doc_id;
  records_by_id_[record.doc_id] = record;
}

void NameIndex::Remove(int64_t doc_id) {
  std::unique_lock<std::shared_mutex> lock(rw_mutex_);
  auto it = records_by_id_.find(doc_id);
  if (it != records_by_id_.end()) {
    id_by_path_.erase(it->second.file_path);
    records_by_id_.erase(it);
  }
}

void NameIndex::RemoveByPath(const std::string& file_path) {
  std::unique_lock<std::shared_mutex> lock(rw_mutex_);
  auto it = id_by_path_.find(file_path);
  if (it != id_by_path_.end()) {
    records_by_id_.erase(it->second);
    id_by_path_.erase(it);
  }
}

bool NameIndex::GetMetadata(int64_t doc_id, uint64_t* out_file_size,
                            int64_t* out_mtime) const {
  std::shared_lock<std::shared_mutex> lock(rw_mutex_);
  auto it = records_by_id_.find(doc_id);
  if (it == records_by_id_.end()) {
    return false;
  }
  if (out_file_size) {
    *out_file_size = it->second.file_size;
  }
  if (out_mtime) {
    *out_mtime = it->second.mtime;
  }
  return true;
}

std::vector<NameMatchResult> NameIndex::Search(
    const std::string& query, int limit, int offset,
    const std::string& path_prefix, int* out_total_count) const {
  if (out_total_count) {
    *out_total_count = 0;
  }
  std::vector<NameMatchResult> results;
  if (query.empty() && path_prefix.empty()) {
    return results;
  }

  std::string lower_query = ToLower(query);
  std::shared_lock<std::shared_mutex> lock(rw_mutex_);

  for (const auto& pair : records_by_id_) {
    const NameRecord& rec = pair.second;

    // Optional path scope constraint
    if (!path_prefix.empty()) {
      if (!StartsWith(rec.file_path, path_prefix)) {
        continue;
      }
    }

    if (lower_query.empty()) {
      // Just path filter
      NameMatchResult res;
      res.doc_id = rec.doc_id;
      res.file_name = rec.file_name;
      res.file_path = rec.file_path;
      res.ext = rec.ext;
      res.file_size = rec.file_size;
      res.mtime = rec.mtime;
      res.score = 1.0;
      results.push_back(std::move(res));
      continue;
    }

    std::string lower_name = ToLower(rec.file_name);
    size_t pos = lower_name.find(lower_query);
    if (pos == std::string::npos) {
      continue;
    }

    double score = 10.0;
    if (lower_name == lower_query) {
      score = 100.0;  // Exact match
    } else if (pos == 0) {
      score = 80.0;   // Prefix match
    } else {
      score = 50.0 - std::min<double>(pos, 30.0);  // Substring match
    }

    NameMatchResult res;
    res.doc_id = rec.doc_id;
    res.file_name = rec.file_name;
    res.file_path = rec.file_path;
    res.ext = rec.ext;
    res.file_size = rec.file_size;
    res.mtime = rec.mtime;
    res.score = score;
    results.push_back(std::move(res));
  }

  // Sort by score descending, then mtime descending
  std::sort(results.begin(), results.end(),
            [](const NameMatchResult& a, const NameMatchResult& b) {
              if (a.score != b.score) {
                return a.score > b.score;
              }
              return a.mtime > b.mtime;
            });

  if (out_total_count) {
    *out_total_count = static_cast<int>(results.size());
  }

  if (offset > 0) {
    if (offset >= static_cast<int>(results.size())) {
      return {};
    }
    results.erase(results.begin(), results.begin() + offset);
  }

  if (static_cast<int>(results.size()) > limit) {
    results.resize(limit);
  }

  return results;
}

size_t NameIndex::size() const {
  std::shared_lock<std::shared_mutex> lock(rw_mutex_);
  return records_by_id_.size();
}

void NameIndex::Clear() {
  std::unique_lock<std::shared_mutex> lock(rw_mutex_);
  records_by_id_.clear();
  id_by_path_.clear();
}

}  // namespace service
}  // namespace soeasy
