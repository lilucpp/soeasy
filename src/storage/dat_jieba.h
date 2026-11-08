// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_STORAGE_DAT_JIEBA_H_
#define SOEASY_SRC_STORAGE_DAT_JIEBA_H_

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cppjieba/HMMModel.hpp"
#include "cppjieba/HMMSegment.hpp"
#include "cppjieba/SegmentBase.hpp"
#include "cppjieba/Unicode.hpp"
#include "darts.h"
#include "storage/mmap_file.h"

namespace soeasy {
namespace storage {

#pragma pack(push, 1)
struct DatHeader {
  char magic[8];       // "DATJIEBA"
  uint32_t version;    // 1
  uint32_t num_words;  // count of words
  double min_weight;   // min log weight
  uint32_t da_size;    // bytes of Darts array
  uint32_t reserved;   // padding
};
#pragma pack(pop)

struct UserWordInfo {
  std::string word;
  double weight;
  std::string tag;
};

/**
 * @brief High-performance Chinese word segmentation engine powered by
 * Double-Array Trie (DAT) + mmap with Overlay dynamic user dictionary.
 *
 * Slashes memory consumption from 120MB down to ~7.2MB with near-instant (0.05ms)
 * startup and zero-copy page cache sharing across GUI and Worker processes.
 */
class DatJieba {
 public:
  DatJieba();
  ~DatJieba() = default;

  // Disable copy semantics
  DatJieba(const DatJieba &) = delete;
  DatJieba &operator=(const DatJieba &) = delete;

  // Enable move semantics
  DatJieba(DatJieba &&) noexcept = default;
  DatJieba &operator=(DatJieba &&) noexcept = default;

  /**
   * @brief Initialize DatJieba by memory-mapping the binary DAT file (jieba_dat.bin)
   * and loading the compact HMM model (50KB).
   *
   * @param dict_dir Directory containing jieba_dat.bin and hmm_model.utf8
   * @return true on success, false on failure
   */
  bool Init(const std::string &dict_dir);

  bool IsInitialized() const {
    return is_initialized_;
  }

  /**
   * @brief Fast existence check for a word in either overlay user dictionary
   * or the base Double-Array Trie dictionary (nanosecond lookup).
   */
  bool HasWord(const std::string &word) const;

  /**
   * @brief Dynamic user word insertion into the Overlay dictionary (thread-safe).
   * Takes effect immediately in subsequent Cut calls without rebuilding DAT.
   */
  bool InsertUserWord(const std::string &word, int freq = 0, const std::string &tag = "");

  /**
   * @brief Dynamic user word removal from the Overlay dictionary (thread-safe).
   */
  bool DeleteUserWord(const std::string &word);

  /**
   * @brief Load user words from a standard text user dictionary file.
   */
  void LoadUserDict(const std::string &user_dict_path);

  /**
   * @brief Main segmentation interface producing Word objects with byte offsets.
   */
  void Cut(const std::string &sentence, std::vector<cppjieba::Word> &words, bool hmm = true) const;

  /**
   * @brief String-only segmentation interface.
   */
  void Cut(const std::string &sentence, std::vector<std::string> &words, bool hmm = true) const;

  bool ResetSeparators(const std::string &s);

  double GetMinWeight() const {
    return min_weight_;
  }

 private:
  struct DagEdge {
    size_t next_rune;
    double weight;
  };

  struct DagNode {
    std::vector<DagEdge> edges;
    double best_weight;
    size_t best_next_rune;
  };

  void CutRange(const std::string &sentence, cppjieba::RuneStrArray::const_iterator begin,
                cppjieba::RuneStrArray::const_iterator end, std::vector<cppjieba::WordRange> &res, bool hmm) const;

  MmapFile mmap_;
  Darts::DoubleArray da_;
  const float *weights_{nullptr};
  double min_weight_{-100.0};
  bool is_initialized_{false};

  // Overlay User Dictionary (thread-safe for dynamic insertion/deletion)
  mutable std::mutex user_dict_mutex_;
  std::unordered_map<std::string, UserWordInfo> user_words_;
  std::unordered_set<cppjieba::Rune> user_single_cjk_;

  std::unique_ptr<cppjieba::HMMModel> hmm_model_;
  std::unique_ptr<cppjieba::HMMSegment> hmm_seg_;
  std::unordered_set<cppjieba::Rune> symbols_;
};

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_DAT_JIEBA_H_
