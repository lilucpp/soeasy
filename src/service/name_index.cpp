#include "service/name_index.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#endif

namespace soeasy {
namespace service {

namespace {

constexpr uint32_t kInvalidU32 = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kHashEmpty = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kHashTombstone = std::numeric_limits<uint32_t>::max() - 1;
constexpr uint32_t kNoDir = std::numeric_limits<uint32_t>::max();
constexpr uint16_t kMaxShortName = std::numeric_limits<uint16_t>::max();
constexpr uint16_t kOverflowExt = std::numeric_limits<uint16_t>::max();
constexpr uint16_t kDirFlagSlashRoot = 1u << 0;
constexpr uint32_t kDirReject = std::numeric_limits<uint32_t>::max();

// Hot metadata scanned during NameIndex::Search (12 bytes: 5.33 entries per 64B cache line).
struct FileHotEntry {
  uint32_t name_offset = 0;
  uint32_t parent_dir_id = kNoDir;  // Doubles as free-list next pointer when deleted == 1
  uint16_t name_len = 0;            // kMaxShortName means check long_basenames_ overflow map
  uint8_t has_upper_ascii = 0;      // Fast-path flag: 0 => already ASCII lowercase
  uint8_t deleted = 0;              // 1 => tombstoned slot
};
static_assert(sizeof(FileHotEntry) == 12, "FileHotEntry must be packed to 12 bytes");
static_assert(alignof(FileHotEntry) == 4, "FileHotEntry alignment must be 4 bytes");

// Cold metadata accessed only on GetMetadata() or final top-K result materialization (24 bytes).
struct FileColdMeta {
  int64_t doc_id = 0;
  uint64_t file_size = 0;
  int64_t mtime = 0;
};
static_assert(sizeof(FileColdMeta) == 24, "FileColdMeta must be packed to 24 bytes");
static_assert(alignof(FileColdMeta) == 8, "FileColdMeta alignment must be 8 bytes");

// Directory trie node stored in parent-before-child topological order (12 bytes).
struct DirNode {
  uint32_t parent_dir_id = kNoDir;
  uint32_t name_offset = 0;
  uint16_t name_len = 0;
  uint16_t flags = 0;
};
static_assert(sizeof(DirNode) == 12, "DirNode must be packed to 12 bytes");
static_assert(alignof(DirNode) == 4, "DirNode alignment must be 4 bytes");

struct SearchCandidate {
  double score = 0.0;
  int64_t mtime = 0;
  uint32_t slot_idx = 0;
};
static_assert(sizeof(SearchCandidate) == 24, "SearchCandidate must be 24 bytes");

inline char AsciiToLowerChar(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline uint8_t DetectUpperAscii(std::string_view s) {
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') {
      return 1;
    }
  }
  return 0;
}

std::string NormalizeSlashes(const std::string &path) {
  std::string out = path;
  std::replace(out.begin(), out.end(), '\\', '/');
  return out;
}

std::string AsciiToLowerString(std::string_view s) {
  std::string out(s);
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + ('a' - 'A'));
    }
  }
  return out;
}

inline size_t FindAsciiCaseInsensitive(std::string_view haystack, std::string_view lower_needle) {
  const size_t nlen = lower_needle.size();
  const size_t hlen = haystack.size();
  if (nlen == 0) return 0;
  if (hlen < nlen) return std::string_view::npos;

  const char first = lower_needle[0];
  const size_t limit = hlen - nlen;
  for (size_t i = 0; i <= limit; ++i) {
    if (AsciiToLowerChar(haystack[i]) != first) {
      continue;
    }
    size_t j = 1;
    while (j < nlen && AsciiToLowerChar(haystack[i + j]) == lower_needle[j]) {
      ++j;
    }
    if (j == nlen) {
      return i;
    }
  }
  return std::string_view::npos;
}

inline uint64_t HashDocId(int64_t doc_id) {
  uint64_t x = static_cast<uint64_t>(doc_id);
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

inline uint64_t HashBytesWithParent(uint32_t parent_id, std::string_view sv) {
  uint64_t h = 14695981039346656037ULL ^ (static_cast<uint64_t>(parent_id) * 0x9e3779b97f4a7c15ULL);
  for (unsigned char ch : sv) {
    h ^= ch;
    h *= 1099511628211ULL;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return h;
}

// SoA open-addressing hash map: int64_t doc_id -> uint32_t slot_idx (12 bytes per bucket).
class DocIdHashMap {
 public:
  uint32_t Find(int64_t key) const {
    if (keys_.empty()) return kInvalidU32;
    const size_t mask = keys_.size() - 1;
    size_t idx = static_cast<size_t>(HashDocId(key)) & mask;
    while (true) {
      const uint32_t v = vals_[idx];
      if (v == kHashEmpty) return kInvalidU32;
      if (v != kHashTombstone && keys_[idx] == key) return v;
      idx = (idx + 1) & mask;
    }
  }

  void InsertOrAssign(int64_t key, uint32_t val) {
    if (keys_.empty()) {
      Rehash(16);
    } else if ((size_ + tombstones_ + 1) * 5 >= keys_.size() * 4) {
      // Only double capacity if live occupancy > 60%; otherwise rehash in-place to purge tombstones.
      const size_t next_cap = ((size_ + 1) * 5 >= keys_.size() * 3) ? (keys_.size() * 2) : keys_.size();
      Rehash(next_cap);
    }

    const size_t mask = keys_.size() - 1;
    size_t idx = static_cast<size_t>(HashDocId(key)) & mask;
    size_t first_tombstone = kInvalidU32;
    while (true) {
      const uint32_t v = vals_[idx];
      if (v == kHashEmpty) {
        const size_t target = (first_tombstone != kInvalidU32) ? first_tombstone : idx;
        if (first_tombstone != kInvalidU32) {
          --tombstones_;
        }
        keys_[target] = key;
        vals_[target] = val;
        ++size_;
        return;
      }
      if (v == kHashTombstone) {
        if (first_tombstone == kInvalidU32) {
          first_tombstone = idx;
        }
      } else if (keys_[idx] == key) {
        vals_[idx] = val;
        return;
      }
      idx = (idx + 1) & mask;
    }
  }

  bool Erase(int64_t key) {
    if (keys_.empty()) return false;
    const size_t mask = keys_.size() - 1;
    size_t idx = static_cast<size_t>(HashDocId(key)) & mask;
    while (true) {
      const uint32_t v = vals_[idx];
      if (v == kHashEmpty) return false;
      if (v != kHashTombstone && keys_[idx] == key) {
        vals_[idx] = kHashTombstone;
        --size_;
        ++tombstones_;
        return true;
      }
      idx = (idx + 1) & mask;
    }
  }

  void Clear() {
    std::vector<int64_t>().swap(keys_);
    std::vector<uint32_t>().swap(vals_);
    size_ = 0;
    tombstones_ = 0;
  }

  void ReserveFor(size_t count) {
    size_t cap = 16;
    while (cap * 4 <= count * 5) {
      cap *= 2;
    }
    if (cap > keys_.size()) {
      Rehash(cap);
    }
  }

  size_t MemoryBytes() const {
    return keys_.capacity() * sizeof(int64_t) + vals_.capacity() * sizeof(uint32_t);
  }

 private:
  void Rehash(size_t new_cap) {
    std::vector<int64_t> old_keys = std::move(keys_);
    std::vector<uint32_t> old_vals = std::move(vals_);
    keys_.assign(new_cap, 0);
    vals_.assign(new_cap, kHashEmpty);
    size_ = 0;
    tombstones_ = 0;

    const size_t mask = new_cap - 1;
    for (size_t i = 0; i < old_vals.size(); ++i) {
      const uint32_t v = old_vals[i];
      if (v == kHashEmpty || v == kHashTombstone) continue;
      size_t idx = static_cast<size_t>(HashDocId(old_keys[i])) & mask;
      while (vals_[idx] != kHashEmpty) {
        idx = (idx + 1) & mask;
      }
      keys_[idx] = old_keys[i];
      vals_[idx] = v;
      ++size_;
    }
  }

  std::vector<int64_t> keys_;
  std::vector<uint32_t> vals_;
  size_t size_ = 0;
  size_t tombstones_ = 0;
};

#ifdef __linux__
// Writer-preferred RW lock prevents 4-reader tight Search() loops from starving Upsert() writers under glibc.
class SharedRwLock {
 public:
  SharedRwLock() {
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&rw_, &attr);
    pthread_rwlockattr_destroy(&attr);
  }
  ~SharedRwLock() { pthread_rwlock_destroy(&rw_); }

  SharedRwLock(const SharedRwLock &) = delete;
  SharedRwLock &operator=(const SharedRwLock &) = delete;

  void lock() { pthread_rwlock_wrlock(&rw_); }
  void unlock() { pthread_rwlock_unlock(&rw_); }
  void lock_shared() { pthread_rwlock_rdlock(&rw_); }
  void unlock_shared() { pthread_rwlock_unlock(&rw_); }

 private:
  pthread_rwlock_t rw_{};
};
#else
using SharedRwLock = std::shared_mutex;
#endif

}  // namespace

struct NameIndex::Impl {
  mutable SharedRwLock rw_mutex;

  // Dense slot arrays (indexed by FileSlotId)
  std::vector<FileHotEntry> hot;
  std::vector<FileColdMeta> cold;
  std::vector<uint16_t> ext_ids;

  // Contiguous arenas (no per-string heap allocations)
  std::string name_arena;
  std::string dir_arena;

  // Directory trie (ordered parent < child)
  std::vector<DirNode> dirs;

  // Extension interning pool
  std::vector<std::string> ext_pool;
  std::unordered_map<std::string, uint16_t> ext_to_id;

  // Rare overflow maps (empty for normal POSIX/Windows filenames)
  std::unordered_map<uint32_t, std::string> custom_file_names;
  std::unordered_map<uint32_t, std::string> long_basenames;
  std::unordered_map<uint32_t, std::string> overflow_exts;
  std::unordered_map<uint32_t, std::string> long_dir_names;

  // Flat hash tables
  DocIdHashMap doc_table;
  std::vector<uint32_t> path_buckets;  // 4 bytes/bucket: slot_idx or Empty/Tombstone
  size_t path_size = 0;
  size_t path_tombstones = 0;

  std::vector<uint32_t> dir_buckets;  // 4 bytes/bucket: dir_id or kHashEmpty
  size_t dir_table_size = 0;

  // Free list and counters
  uint32_t free_head = kInvalidU32;
  size_t active_count = 0;
  size_t dead_name_bytes = 0;

  Impl() {
    ext_pool.emplace_back("");
    ext_to_id.emplace("", static_cast<uint16_t>(0));
  }

  std::string_view GetBasename(uint32_t slot_idx) const {
    const FileHotEntry &entry = hot[slot_idx];
    if (entry.name_len == kMaxShortName) {
      auto it = long_basenames.find(slot_idx);
      if (it != long_basenames.end()) {
        return it->second;
      }
    }
    return std::string_view(name_arena.data() + entry.name_offset, entry.name_len);
  }

  std::string_view GetDirSegmentName(uint32_t dir_id) const {
    const DirNode &node = dirs[dir_id];
    if (node.name_len == kMaxShortName) {
      auto it = long_dir_names.find(dir_id);
      if (it != long_dir_names.end()) {
        return it->second;
      }
    }
    return std::string_view(dir_arena.data() + node.name_offset, node.name_len);
  }

  std::string_view GetSearchFileName(uint32_t slot_idx) const {
    if (!custom_file_names.empty()) {
      auto it = custom_file_names.find(slot_idx);
      if (it != custom_file_names.end()) {
        return it->second;
      }
    }
    return GetBasename(slot_idx);
  }

  std::string_view GetExtension(uint32_t slot_idx) const {
    const uint16_t eid = ext_ids[slot_idx];
    if (eid == kOverflowExt) {
      auto it = overflow_exts.find(slot_idx);
      return (it != overflow_exts.end()) ? std::string_view(it->second) : std::string_view();
    }
    return ext_pool[eid];
  }

  uint16_t InternExt(const std::string &ext, uint32_t slot_idx) {
    overflow_exts.erase(slot_idx);
    if (ext.empty()) {
      return 0;
    }
    auto it = ext_to_id.find(ext);
    if (it != ext_to_id.end()) {
      return it->second;
    }
    if (ext_pool.size() < kOverflowExt) {
      const uint16_t id = static_cast<uint16_t>(ext_pool.size());
      ext_pool.push_back(ext);
      ext_to_id.emplace(ext, id);
      return id;
    }
    overflow_exts[slot_idx] = ext;
    return kOverflowExt;
  }

  // --- Directory Hash Table ---
  void RehashDirTable(size_t new_cap) {
    dir_buckets.assign(new_cap, kHashEmpty);
    dir_table_size = 0;
    const size_t mask = new_cap - 1;
    for (uint32_t dir_id = 0; dir_id < dirs.size(); ++dir_id) {
      const DirNode &node = dirs[dir_id];
      const std::string_view seg = GetDirSegmentName(dir_id);
      size_t idx = static_cast<size_t>(HashBytesWithParent(node.parent_dir_id, seg)) & mask;
      while (dir_buckets[idx] != kHashEmpty) {
        idx = (idx + 1) & mask;
      }
      dir_buckets[idx] = dir_id;
      ++dir_table_size;
    }
  }

  uint32_t InternOrFindDirSegment(uint32_t parent_id, std::string_view seg, uint16_t flags, bool create) {
    if (!dir_buckets.empty()) {
      const size_t mask = dir_buckets.size() - 1;
      size_t idx = static_cast<size_t>(HashBytesWithParent(parent_id, seg)) & mask;
      while (true) {
        const uint32_t dir_id = dir_buckets[idx];
        if (dir_id == kHashEmpty) break;
        const DirNode &node = dirs[dir_id];
        if (node.parent_dir_id == parent_id && node.flags == flags && GetDirSegmentName(dir_id) == seg) {
          return dir_id;
        }
        idx = (idx + 1) & mask;
      }
    }

    if (!create) {
      return kInvalidU32;
    }

    if (dir_buckets.empty()) {
      RehashDirTable(16);
    } else if ((dir_table_size + 1) * 5 >= dir_buckets.size() * 4) {
      RehashDirTable(dir_buckets.size() * 2);
    }

    const uint32_t new_id = static_cast<uint32_t>(dirs.size());
    DirNode node;
    node.parent_dir_id = parent_id;
    node.flags = flags;
    if (seg.size() >= kMaxShortName) {
      node.name_offset = 0;
      node.name_len = kMaxShortName;
      long_dir_names.emplace(new_id, std::string(seg));
    } else {
      node.name_offset = static_cast<uint32_t>(dir_arena.size());
      node.name_len = static_cast<uint16_t>(seg.size());
      dir_arena.append(seg.data(), seg.size());
    }
    dirs.push_back(node);

    const size_t mask = dir_buckets.size() - 1;
    size_t idx = static_cast<size_t>(HashBytesWithParent(parent_id, seg)) & mask;
    while (dir_buckets[idx] != kHashEmpty) {
      idx = (idx + 1) & mask;
    }
    dir_buckets[idx] = new_id;
    ++dir_table_size;
    return new_id;
  }

  bool ResolveNormalizedPath(std::string_view norm_path, bool create_dirs, uint32_t *out_parent_dir,
                             std::string_view *out_basename) {
    const size_t last_slash = norm_path.rfind('/');
    if (last_slash == std::string_view::npos) {
      *out_parent_dir = kNoDir;
      *out_basename = norm_path;
      return true;
    }

    *out_basename = norm_path.substr(last_slash + 1);
    uint32_t cur = kNoDir;
    size_t pos = 0;
    if (norm_path[0] == '/') {
      cur = InternOrFindDirSegment(cur, "/", kDirFlagSlashRoot, create_dirs);
      if (cur == kInvalidU32) return false;
      if (last_slash == 0) {
        *out_parent_dir = cur;
        return true;
      }
      pos = 1;
    }

    const std::string_view dir_body(norm_path.data(), last_slash);
    while (true) {
      const size_t next = dir_body.find('/', pos);
      if (next == std::string_view::npos) {
        const std::string_view seg = dir_body.substr(pos);
        cur = InternOrFindDirSegment(cur, seg, 0, create_dirs);
        if (cur == kInvalidU32) return false;
        break;
      }
      const std::string_view seg = dir_body.substr(pos, next - pos);
      cur = InternOrFindDirSegment(cur, seg, 0, create_dirs);
      if (cur == kInvalidU32) return false;
      pos = next + 1;
    }

    *out_parent_dir = cur;
    return true;
  }

  std::string ReconstructPath(uint32_t dir_id, std::string_view basename) const {
    if (dir_id == kNoDir) {
      return std::string(basename);
    }

    std::array<uint32_t, 64> stack_chain{};
    size_t depth = 0;
    std::vector<uint32_t> heap_chain;

    uint32_t cur = dir_id;
    while (cur != kNoDir) {
      if (heap_chain.empty() && depth < stack_chain.size()) {
        stack_chain[depth++] = cur;
      } else {
        if (heap_chain.empty()) {
          heap_chain.assign(stack_chain.begin(), stack_chain.end());
        }
        heap_chain.push_back(cur);
      }
      cur = dirs[cur].parent_dir_id;
    }

    const uint32_t *chain_ptr = heap_chain.empty() ? stack_chain.data() : heap_chain.data();
    const size_t chain_len = heap_chain.empty() ? depth : heap_chain.size();

    size_t total_len = basename.size() + chain_len + 1;
    for (size_t i = 0; i < chain_len; ++i) {
      total_len += GetDirSegmentName(chain_ptr[i]).size();
    }

    std::string out;
    out.reserve(total_len);

    const uint32_t root_id = chain_ptr[chain_len - 1];
    const bool root_is_slash = (dirs[root_id].flags & kDirFlagSlashRoot) != 0;
    const std::string_view root_name = GetDirSegmentName(root_id);
    out.append(root_name.data(), root_name.size());

    for (size_t i = chain_len - 1; i-- > 0;) {
      if (!(i == chain_len - 2 && root_is_slash)) {
        out.push_back('/');
      }
      const std::string_view seg = GetDirSegmentName(chain_ptr[i]);
      out.append(seg.data(), seg.size());
    }

    if (!(chain_len == 1 && root_is_slash)) {
      out.push_back('/');
    }
    out.append(basename.data(), basename.size());
    return out;
  }

  // --- Path Hash Table ---
  void RehashPathTable(size_t new_cap) {
    std::vector<uint32_t> old_buckets = std::move(path_buckets);
    path_buckets.assign(new_cap, kHashEmpty);
    path_size = 0;
    path_tombstones = 0;

    const size_t mask = new_cap - 1;
    for (uint32_t slot_idx : old_buckets) {
      if (slot_idx == kHashEmpty || slot_idx == kHashTombstone) continue;
      if (hot[slot_idx].deleted) continue;
      const std::string_view base = GetBasename(slot_idx);
      size_t idx = static_cast<size_t>(HashBytesWithParent(hot[slot_idx].parent_dir_id, base)) & mask;
      while (path_buckets[idx] != kHashEmpty) {
        idx = (idx + 1) & mask;
      }
      path_buckets[idx] = slot_idx;
      ++path_size;
    }
  }

  uint32_t FindSlotByKey(uint32_t parent_dir_id, std::string_view basename) const {
    if (path_buckets.empty()) return kInvalidU32;
    const size_t mask = path_buckets.size() - 1;
    size_t idx = static_cast<size_t>(HashBytesWithParent(parent_dir_id, basename)) & mask;
    while (true) {
      const uint32_t slot_idx = path_buckets[idx];
      if (slot_idx == kHashEmpty) return kInvalidU32;
      if (slot_idx != kHashTombstone) {
        const FileHotEntry &entry = hot[slot_idx];
        if (!entry.deleted && entry.parent_dir_id == parent_dir_id && GetBasename(slot_idx) == basename) {
          return slot_idx;
        }
      }
      idx = (idx + 1) & mask;
    }
  }

  void InsertPathSlot(uint32_t slot_idx) {
    if (path_buckets.empty()) {
      RehashPathTable(16);
    } else if ((path_size + path_tombstones + 1) * 5 >= path_buckets.size() * 4) {
      // Only double capacity if live occupancy > 60%; otherwise rehash in-place to purge tombstones.
      const size_t next_cap =
          ((path_size + 1) * 5 >= path_buckets.size() * 3) ? (path_buckets.size() * 2) : path_buckets.size();
      RehashPathTable(next_cap);
    }

    const uint32_t parent_dir_id = hot[slot_idx].parent_dir_id;
    const std::string_view base = GetBasename(slot_idx);
    const size_t mask = path_buckets.size() - 1;
    size_t idx = static_cast<size_t>(HashBytesWithParent(parent_dir_id, base)) & mask;
    size_t first_tombstone = kInvalidU32;
    while (true) {
      const uint32_t cur = path_buckets[idx];
      if (cur == kHashEmpty) {
        const size_t target = (first_tombstone != kInvalidU32) ? first_tombstone : idx;
        if (first_tombstone != kInvalidU32) {
          --path_tombstones;
        }
        path_buckets[target] = slot_idx;
        ++path_size;
        return;
      }
      if (cur == kHashTombstone && first_tombstone == kInvalidU32) {
        first_tombstone = idx;
      }
      idx = (idx + 1) & mask;
    }
  }

  void ErasePathKey(uint32_t slot_idx, uint32_t parent_dir_id, std::string_view basename) {
    if (path_buckets.empty()) return;
    const size_t mask = path_buckets.size() - 1;
    size_t idx = static_cast<size_t>(HashBytesWithParent(parent_dir_id, basename)) & mask;
    while (true) {
      const uint32_t cur = path_buckets[idx];
      if (cur == kHashEmpty) return;
      if (cur == slot_idx) {
        path_buckets[idx] = kHashTombstone;
        --path_size;
        ++path_tombstones;
        return;
      }
      idx = (idx + 1) & mask;
    }
  }

  void EraseSlotInternal(uint32_t slot_idx) {
    FileHotEntry &entry = hot[slot_idx];
    if (entry.deleted) return;

    const std::string_view base = GetBasename(slot_idx);
    ErasePathKey(slot_idx, entry.parent_dir_id, base);
    doc_table.Erase(cold[slot_idx].doc_id);

    if (entry.name_len != kMaxShortName) {
      dead_name_bytes += entry.name_len;
    }
    long_basenames.erase(slot_idx);
    custom_file_names.erase(slot_idx);
    overflow_exts.erase(slot_idx);

    entry.deleted = 1;
    entry.parent_dir_id = free_head;
    entry.name_offset = 0;
    entry.name_len = 0;
    entry.has_upper_ascii = 0;
    free_head = slot_idx;
    --active_count;
  }

  void SetSlotBasename(uint32_t slot_idx, std::string_view basename, const std::string &record_file_name) {
    FileHotEntry &entry = hot[slot_idx];
    long_basenames.erase(slot_idx);
    if (basename.size() >= kMaxShortName) {
      entry.name_offset = 0;
      entry.name_len = kMaxShortName;
      long_basenames.emplace(slot_idx, std::string(basename));
    } else {
      entry.name_offset = static_cast<uint32_t>(name_arena.size());
      entry.name_len = static_cast<uint16_t>(basename.size());
      name_arena.append(basename.data(), basename.size());
    }

    if (basename == record_file_name) {
      custom_file_names.erase(slot_idx);
      entry.has_upper_ascii = DetectUpperAscii(basename);
    } else {
      custom_file_names[slot_idx] = record_file_name;
      entry.has_upper_ascii = DetectUpperAscii(record_file_name);
    }
  }

  // Streaming prefix state machine over topologically sorted DirNode vector.
  // Returns b_state[dir_id] = bytes matched of norm_prefix by B(dir_id), or kDirReject.
  void ComputeDirPrefixStates(std::string_view prefix, std::vector<uint32_t> *out_b_state) const {
    const size_t num_dirs = dirs.size();
    const uint32_t plen = static_cast<uint32_t>(prefix.size());
    out_b_state->resize(num_dirs);
    if (num_dirs == 0) return;

    std::vector<uint32_t> d_state(num_dirs);
    for (size_t i = 0; i < num_dirs; ++i) {
      const DirNode &node = dirs[i];
      const std::string_view seg = GetDirSegmentName(static_cast<uint32_t>(i));
      const bool is_slash_root = (node.flags & kDirFlagSlashRoot) != 0;

      uint32_t ds = kDirReject;
      if (node.parent_dir_id == kNoDir) {
        const size_t cmp_len = std::min(seg.size(), prefix.size());
        if (std::memcmp(seg.data(), prefix.data(), cmp_len) == 0) {
          ds = (seg.size() >= plen) ? plen : static_cast<uint32_t>(seg.size());
        }
      } else {
        const uint32_t p_ds = d_state[node.parent_dir_id];
        if (p_ds == kDirReject) {
          ds = kDirReject;
        } else if (p_ds == plen) {
          ds = plen;
        } else {
          uint32_t cursor = p_ds;
          const bool parent_is_slash_root = (dirs[node.parent_dir_id].flags & kDirFlagSlashRoot) != 0;
          bool ok = true;
          if (!parent_is_slash_root) {
            if (prefix[cursor] != '/') {
              ok = false;
            } else {
              ++cursor;
            }
          }
          if (ok) {
            if (cursor >= plen) {
              ds = plen;
            } else {
              const size_t rem = plen - cursor;
              const size_t cmp_len = std::min(seg.size(), rem);
              if (std::memcmp(seg.data(), prefix.data() + cursor, cmp_len) == 0) {
                const size_t matched = cursor + seg.size();
                ds = (matched >= plen) ? plen : static_cast<uint32_t>(matched);
              }
            }
          }
        }
      }

      d_state[i] = ds;

      uint32_t bs = kDirReject;
      if (ds != kDirReject) {
        if (ds == plen || is_slash_root) {
          bs = ds;
        } else if (prefix[ds] == '/') {
          bs = ds + 1;
        }
      }
      (*out_b_state)[i] = bs;
    }
  }

  inline bool MatchesPrefixStates(uint32_t slot_idx, const FileHotEntry &entry, std::string_view prefix,
                                  const std::vector<uint32_t> &b_state) const {
    if (entry.parent_dir_id == kNoDir) {
      const std::string_view base = GetBasename(slot_idx);
      return base.size() >= prefix.size() && std::memcmp(base.data(), prefix.data(), prefix.size()) == 0;
    }
    const uint32_t bs = b_state[entry.parent_dir_id];
    if (bs == kDirReject) return false;
    if (bs == prefix.size()) return true;
    const size_t rem = prefix.size() - bs;
    const std::string_view base = GetBasename(slot_idx);
    return base.size() >= rem && std::memcmp(base.data(), prefix.data() + bs, rem) == 0;
  }

  void Compact() {
    if (active_count == 0) {
      ClearAll();
      return;
    }

    // 1. Mark reachable directories bottom-up (guaranteed parent < child).
    std::vector<uint8_t> used_dir(dirs.size(), 0);
    for (const FileHotEntry &entry : hot) {
      if (!entry.deleted && entry.parent_dir_id != kNoDir) {
        used_dir[entry.parent_dir_id] = 1;
      }
    }
    size_t active_dir_count = 0;
    size_t active_dir_bytes = 0;
    for (size_t i = dirs.size(); i-- > 0;) {
      if (!used_dir[i]) continue;
      ++active_dir_count;
      if (dirs[i].name_len != kMaxShortName) {
        active_dir_bytes += dirs[i].name_len;
      }
      if (dirs[i].parent_dir_id != kNoDir) {
        used_dir[dirs[i].parent_dir_id] = 1;
      }
    }

    std::vector<uint32_t> dir_remap(dirs.size(), kNoDir);
    std::vector<DirNode> new_dirs;
    new_dirs.reserve(active_dir_count);
    std::string new_dir_arena;
    new_dir_arena.reserve(active_dir_bytes);
    std::unordered_map<uint32_t, std::string> new_long_dir_names;

    for (uint32_t old_id = 0; old_id < dirs.size(); ++old_id) {
      if (!used_dir[old_id]) continue;
      const DirNode &old_node = dirs[old_id];
      const uint32_t new_id = static_cast<uint32_t>(new_dirs.size());
      dir_remap[old_id] = new_id;

      DirNode nn;
      nn.parent_dir_id = (old_node.parent_dir_id == kNoDir) ? kNoDir : dir_remap[old_node.parent_dir_id];
      nn.flags = old_node.flags;
      const std::string_view seg = GetDirSegmentName(old_id);
      if (seg.size() >= kMaxShortName) {
        nn.name_offset = 0;
        nn.name_len = kMaxShortName;
        new_long_dir_names.emplace(new_id, std::string(seg));
      } else {
        nn.name_offset = static_cast<uint32_t>(new_dir_arena.size());
        nn.name_len = static_cast<uint16_t>(seg.size());
        new_dir_arena.append(seg.data(), seg.size());
      }
      new_dirs.push_back(nn);
    }

    // 2. Compact live file slots and name_arena.
    std::vector<FileHotEntry> new_hot;
    std::vector<FileColdMeta> new_cold;
    std::vector<uint16_t> new_ext_ids;
    new_hot.reserve(active_count);
    new_cold.reserve(active_count);
    new_ext_ids.reserve(active_count);

    std::string new_name_arena;
    if (name_arena.size() > dead_name_bytes) {
      new_name_arena.reserve(name_arena.size() - dead_name_bytes);
    }

    std::unordered_map<uint32_t, std::string> new_custom_names;
    std::unordered_map<uint32_t, std::string> new_long_basenames;
    std::unordered_map<uint32_t, std::string> new_overflow_exts;

    for (uint32_t old_slot = 0; old_slot < hot.size(); ++old_slot) {
      const FileHotEntry &old_entry = hot[old_slot];
      if (old_entry.deleted) continue;

      const uint32_t new_slot = static_cast<uint32_t>(new_hot.size());
      FileHotEntry ne = old_entry;
      ne.parent_dir_id = (old_entry.parent_dir_id == kNoDir) ? kNoDir : dir_remap[old_entry.parent_dir_id];

      const std::string_view base = GetBasename(old_slot);
      if (base.size() >= kMaxShortName) {
        ne.name_offset = 0;
        ne.name_len = kMaxShortName;
        new_long_basenames.emplace(new_slot, std::string(base));
      } else {
        ne.name_offset = static_cast<uint32_t>(new_name_arena.size());
        ne.name_len = static_cast<uint16_t>(base.size());
        new_name_arena.append(base.data(), base.size());
      }

      if (!custom_file_names.empty()) {
        auto it = custom_file_names.find(old_slot);
        if (it != custom_file_names.end()) {
          new_custom_names.emplace(new_slot, std::move(it->second));
        }
      }
      if (ext_ids[old_slot] == kOverflowExt) {
        auto it = overflow_exts.find(old_slot);
        if (it != overflow_exts.end()) {
          new_overflow_exts.emplace(new_slot, std::move(it->second));
        }
      }

      new_hot.push_back(ne);
      new_cold.push_back(cold[old_slot]);
      new_ext_ids.push_back(ext_ids[old_slot]);
    }

    hot.swap(new_hot);
    cold.swap(new_cold);
    ext_ids.swap(new_ext_ids);
    name_arena.swap(new_name_arena);
    dir_arena.swap(new_dir_arena);
    dirs.swap(new_dirs);
    custom_file_names.swap(new_custom_names);
    long_basenames.swap(new_long_basenames);
    overflow_exts.swap(new_overflow_exts);
    long_dir_names.swap(new_long_dir_names);
    free_head = kInvalidU32;
    dead_name_bytes = 0;

    // 3. Rebuild flat hash tables cleanly with zero tombstones.
    size_t dir_cap = 16;
    while (dir_cap * 4 <= dirs.size() * 5) {
      dir_cap *= 2;
    }
    RehashDirTable(dir_cap);

    doc_table.Clear();
    doc_table.ReserveFor(hot.size());
    for (uint32_t s = 0; s < hot.size(); ++s) {
      doc_table.InsertOrAssign(cold[s].doc_id, s);
    }

    size_t path_cap = 16;
    while (path_cap * 4 <= hot.size() * 5) {
      path_cap *= 2;
    }
    path_buckets.assign(path_cap, kHashEmpty);
    path_size = 0;
    path_tombstones = 0;
    const size_t mask = path_cap - 1;
    for (uint32_t s = 0; s < hot.size(); ++s) {
      const std::string_view base = GetBasename(s);
      size_t idx = static_cast<size_t>(HashBytesWithParent(hot[s].parent_dir_id, base)) & mask;
      while (path_buckets[idx] != kHashEmpty) {
        idx = (idx + 1) & mask;
      }
      path_buckets[idx] = s;
      ++path_size;
    }
  }

  void MaybeCompact() {
    if (active_count == 0) {
      if (!hot.empty()) {
        ClearAll();
      }
      return;
    }
    const size_t tombstones = hot.size() - active_count;
    const bool high_slot_waste = (hot.size() > 256 && tombstones * 4 > hot.size());
    const bool high_arena_waste = (dead_name_bytes > 65536 && dead_name_bytes * 2 > name_arena.size());
    if (high_slot_waste || high_arena_waste) {
      Compact();
    }
  }

  void ClearAll() {
    std::vector<FileHotEntry>().swap(hot);
    std::vector<FileColdMeta>().swap(cold);
    std::vector<uint16_t>().swap(ext_ids);
    std::string().swap(name_arena);
    std::string().swap(dir_arena);
    std::vector<DirNode>().swap(dirs);

    ext_pool.clear();
    ext_pool.emplace_back("");
    ext_to_id.clear();
    ext_to_id.emplace("", static_cast<uint16_t>(0));

    custom_file_names.clear();
    long_basenames.clear();
    overflow_exts.clear();
    long_dir_names.clear();

    doc_table.Clear();
    std::vector<uint32_t>().swap(path_buckets);
    path_size = 0;
    path_tombstones = 0;
    std::vector<uint32_t>().swap(dir_buckets);
    dir_table_size = 0;

    free_head = kInvalidU32;
    active_count = 0;
    dead_name_bytes = 0;
  }
};

NameIndex::NameIndex() : impl_(std::make_unique<Impl>()) {}
NameIndex::~NameIndex() = default;

void NameIndex::Upsert(const NameRecord &record) {
  const std::string norm_path = NormalizeSlashes(record.file_path);

  std::unique_lock<SharedRwLock> lock(impl_->rw_mutex);

  uint32_t parent_dir_id = kNoDir;
  std::string_view basename;
  impl_->ResolveNormalizedPath(norm_path, /*create_dirs=*/true, &parent_dir_id, &basename);

  // If another doc_id currently owns this normalized path, remove that stale doc_id first.
  const uint32_t path_owner_slot = impl_->FindSlotByKey(parent_dir_id, basename);
  if (path_owner_slot != kInvalidU32 && impl_->cold[path_owner_slot].doc_id != record.doc_id) {
    impl_->EraseSlotInternal(path_owner_slot);
  }

  uint32_t slot_idx = impl_->doc_table.Find(record.doc_id);
  if (slot_idx != kInvalidU32) {
    // Updating existing doc_id in-place.
    FileHotEntry &entry = impl_->hot[slot_idx];
    const uint32_t old_parent_dir = entry.parent_dir_id;
    const std::string_view old_basename = impl_->GetBasename(slot_idx);
    const bool same_path = (old_parent_dir == parent_dir_id && old_basename == basename);

    if (!same_path) {
      impl_->ErasePathKey(slot_idx, old_parent_dir, old_basename);
      if (entry.name_len != kMaxShortName) {
        impl_->dead_name_bytes += entry.name_len;
      }
      entry.parent_dir_id = parent_dir_id;
      impl_->SetSlotBasename(slot_idx, basename, record.file_name);
      impl_->InsertPathSlot(slot_idx);
    } else if (basename != record.file_name) {
      impl_->custom_file_names[slot_idx] = record.file_name;
      entry.has_upper_ascii = DetectUpperAscii(record.file_name);
    } else if (!impl_->custom_file_names.empty()) {
      impl_->custom_file_names.erase(slot_idx);
      entry.has_upper_ascii = DetectUpperAscii(basename);
    }

    impl_->cold[slot_idx] = FileColdMeta{record.doc_id, record.file_size, record.mtime};
    impl_->ext_ids[slot_idx] = impl_->InternExt(record.ext, slot_idx);
    impl_->MaybeCompact();
    return;
  }

  // Allocate a new or recycled slot.
  if (impl_->free_head != kInvalidU32) {
    slot_idx = impl_->free_head;
    impl_->free_head = impl_->hot[slot_idx].parent_dir_id;
  } else {
    slot_idx = static_cast<uint32_t>(impl_->hot.size());
    impl_->hot.emplace_back();
    impl_->cold.emplace_back();
    impl_->ext_ids.emplace_back(0);
  }

  FileHotEntry &entry = impl_->hot[slot_idx];
  entry.parent_dir_id = parent_dir_id;
  entry.deleted = 0;
  impl_->SetSlotBasename(slot_idx, basename, record.file_name);

  impl_->cold[slot_idx] = FileColdMeta{record.doc_id, record.file_size, record.mtime};
  impl_->ext_ids[slot_idx] = impl_->InternExt(record.ext, slot_idx);
  ++impl_->active_count;

  impl_->doc_table.InsertOrAssign(record.doc_id, slot_idx);
  impl_->InsertPathSlot(slot_idx);
}

void NameIndex::Remove(int64_t doc_id) {
  std::unique_lock<SharedRwLock> lock(impl_->rw_mutex);
  const uint32_t slot_idx = impl_->doc_table.Find(doc_id);
  if (slot_idx == kInvalidU32) return;
  impl_->EraseSlotInternal(slot_idx);
  impl_->MaybeCompact();
}

void NameIndex::RemoveByPath(const std::string &file_path) {
  const std::string norm_path = NormalizeSlashes(file_path);

  std::unique_lock<SharedRwLock> lock(impl_->rw_mutex);
  uint32_t parent_dir_id = kNoDir;
  std::string_view basename;
  if (!impl_->ResolveNormalizedPath(norm_path, /*create_dirs=*/false, &parent_dir_id, &basename)) {
    return;
  }
  const uint32_t slot_idx = impl_->FindSlotByKey(parent_dir_id, basename);
  if (slot_idx == kInvalidU32) return;
  impl_->EraseSlotInternal(slot_idx);
  impl_->MaybeCompact();
}

void NameIndex::RemoveBatch(const std::vector<int64_t> &doc_ids) {
  if (doc_ids.empty()) return;
  std::unique_lock<SharedRwLock> lock(impl_->rw_mutex);
  for (int64_t id : doc_ids) {
    const uint32_t slot_idx = impl_->doc_table.Find(id);
    if (slot_idx != kInvalidU32) {
      impl_->EraseSlotInternal(slot_idx);
    }
  }
  impl_->MaybeCompact();
}

void NameIndex::RemoveDirectoryPrefix(const std::string &dir_prefix) {
  std::string normalized = NormalizeSlashes(dir_prefix);
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  const std::string prefix_slash = normalized + "/";

  std::unique_lock<SharedRwLock> lock(impl_->rw_mutex);
  if (impl_->active_count == 0) return;

  // 1. If a record's exact normalized path equals normalized, remove it.
  {
    uint32_t exact_parent = kNoDir;
    std::string_view exact_base;
    if (impl_->ResolveNormalizedPath(normalized, /*create_dirs=*/false, &exact_parent, &exact_base)) {
      const uint32_t exact_slot = impl_->FindSlotByKey(exact_parent, exact_base);
      if (exact_slot != kInvalidU32) {
        impl_->EraseSlotInternal(exact_slot);
      }
    }
  }

  // 2. Remove every file whose path starts with (normalized + "/").
  std::vector<uint32_t> b_state;
  impl_->ComputeDirPrefixStates(prefix_slash, &b_state);

  const uint32_t num_slots = static_cast<uint32_t>(impl_->hot.size());
  for (uint32_t s = 0; s < num_slots; ++s) {
    const FileHotEntry &entry = impl_->hot[s];
    if (!entry.deleted && impl_->MatchesPrefixStates(s, entry, prefix_slash, b_state)) {
      impl_->EraseSlotInternal(s);
    }
  }

  impl_->MaybeCompact();
}

bool NameIndex::GetMetadata(int64_t doc_id, uint64_t *out_file_size, int64_t *out_mtime) const {
  std::shared_lock<SharedRwLock> lock(impl_->rw_mutex);
  const uint32_t slot_idx = impl_->doc_table.Find(doc_id);
  if (slot_idx == kInvalidU32) {
    return false;
  }
  const FileColdMeta &meta = impl_->cold[slot_idx];
  if (out_file_size) *out_file_size = meta.file_size;
  if (out_mtime) *out_mtime = meta.mtime;
  return true;
}

std::vector<NameMatchResult> NameIndex::Search(const std::string &query, int limit, int offset,
                                               const std::string &path_prefix, int *out_total_count) const {
  if (query.empty() && path_prefix.empty()) {
    if (out_total_count) *out_total_count = 0;
    return {};
  }

  const std::string lower_query = AsciiToLowerString(query);
  const std::string norm_prefix = NormalizeSlashes(path_prefix);
  const bool has_prefix = !norm_prefix.empty();
  const bool has_query = !lower_query.empty();
  const size_t qlen = lower_query.size();

  std::shared_lock<SharedRwLock> lock(impl_->rw_mutex);

  std::vector<uint32_t> b_state;
  if (has_prefix) {
    impl_->ComputeDirPrefixStates(norm_prefix, &b_state);
  }

  std::vector<SearchCandidate> candidates;
  const bool has_custom_names = !impl_->custom_file_names.empty();
  if (impl_->active_count > 0) {
    candidates.reserve(std::min<size_t>(impl_->active_count, 1024));
  }

  const uint32_t num_slots = static_cast<uint32_t>(impl_->hot.size());
  for (uint32_t s = 0; s < num_slots; ++s) {
    const FileHotEntry &entry = impl_->hot[s];
    if (entry.deleted) continue;

    if (has_prefix && !impl_->MatchesPrefixStates(s, entry, norm_prefix, b_state)) {
      continue;
    }

    double score = 1.0;
    if (has_query) {
      if (!has_custom_names && entry.name_len != kMaxShortName && entry.name_len < qlen) {
        continue;
      }

      const std::string_view fname = impl_->GetSearchFileName(s);
      size_t pos = std::string_view::npos;
      if (!entry.has_upper_ascii && entry.name_len != kMaxShortName && !has_custom_names) {
        pos = fname.find(lower_query);
      } else {
        pos = FindAsciiCaseInsensitive(fname, lower_query);
      }

      if (pos == std::string_view::npos) {
        continue;
      }
      if (fname.size() == qlen) {
        score = 100.0;
      } else if (pos == 0) {
        score = 80.0;
      } else {
        score = 50.0 - std::min<double>(static_cast<double>(pos), 30.0);
      }
    }

    candidates.push_back(SearchCandidate{score, impl_->cold[s].mtime, s});
  }

  const size_t total_matches = candidates.size();
  if (out_total_count) {
    *out_total_count = static_cast<int>(std::min<size_t>(total_matches, std::numeric_limits<int>::max()));
  }

  if (limit <= 0) {
    return {};
  }
  const size_t safe_offset = (offset > 0) ? static_cast<size_t>(offset) : 0;
  if (safe_offset >= total_matches) {
    return {};
  }

  const size_t avail = total_matches - safe_offset;
  const size_t take = std::min(avail, static_cast<size_t>(limit));
  const size_t end_idx = safe_offset + take;

  auto cmp = [](const SearchCandidate &a, const SearchCandidate &b) {
    if (a.score != b.score) return a.score > b.score;
    return a.mtime > b.mtime;
  };

  if (end_idx < total_matches) {
    std::partial_sort(candidates.begin(), candidates.begin() + end_idx, candidates.end(), cmp);
  } else {
    std::sort(candidates.begin(), candidates.end(), cmp);
  }

  std::vector<NameMatchResult> results;
  results.reserve(take);
  for (size_t i = safe_offset; i < end_idx; ++i) {
    const uint32_t s = candidates[i].slot_idx;
    const FileColdMeta &meta = impl_->cold[s];
    const std::string_view base = impl_->GetBasename(s);
    const std::string_view fname = impl_->GetSearchFileName(s);
    const std::string_view ext = impl_->GetExtension(s);

    NameMatchResult item;
    item.doc_id = meta.doc_id;
    item.file_name.assign(fname.data(), fname.size());
    item.file_path = impl_->ReconstructPath(impl_->hot[s].parent_dir_id, base);
    item.ext.assign(ext.data(), ext.size());
    item.file_size = meta.file_size;
    item.mtime = meta.mtime;
    item.score = candidates[i].score;
    results.push_back(std::move(item));
  }
  return results;
}

size_t NameIndex::size() const {
  std::shared_lock<SharedRwLock> lock(impl_->rw_mutex);
  return impl_->active_count;
}

void NameIndex::Clear() {
  std::unique_lock<SharedRwLock> lock(impl_->rw_mutex);
  impl_->ClearAll();
}

size_t NameIndex::ApproximateMemoryUsage() const {
  auto map_string_bytes = [](const auto &m) -> size_t {
    size_t total = 0;
    for (const auto &kv : m) {
      total += 32 + kv.second.capacity();
    }
    return total;
  };

  std::shared_lock<SharedRwLock> lock(impl_->rw_mutex);
  size_t bytes = sizeof(*this) + sizeof(Impl);
  bytes += impl_->hot.capacity() * sizeof(FileHotEntry);
  bytes += impl_->cold.capacity() * sizeof(FileColdMeta);
  bytes += impl_->ext_ids.capacity() * sizeof(uint16_t);
  bytes += impl_->name_arena.capacity();
  bytes += impl_->dir_arena.capacity();
  bytes += impl_->dirs.capacity() * sizeof(DirNode);
  bytes += impl_->ext_pool.capacity() * sizeof(std::string);
  for (const auto &ext : impl_->ext_pool) {
    bytes += ext.capacity();
  }
  for (const auto &kv : impl_->ext_to_id) {
    bytes += 32 + kv.first.capacity();
  }
  bytes += map_string_bytes(impl_->custom_file_names);
  bytes += map_string_bytes(impl_->long_basenames);
  bytes += map_string_bytes(impl_->overflow_exts);
  bytes += map_string_bytes(impl_->long_dir_names);
  bytes += impl_->doc_table.MemoryBytes();
  bytes += impl_->path_buckets.capacity() * sizeof(uint32_t);
  bytes += impl_->dir_buckets.capacity() * sizeof(uint32_t);
  return bytes;
}

}  // namespace service
}  // namespace soeasy
