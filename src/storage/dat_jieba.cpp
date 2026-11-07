// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "storage/dat_jieba.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "common/logger.h"
#include "cppjieba/PreFilter.hpp"
#include "utf8.h"

namespace soeasy {
namespace storage {

namespace {

bool CompileTextDictToDat(const std::string &input_path, const std::string &output_path) {
  std::ifstream ifs(input_path);
  if (!ifs.is_open()) {
    return false;
  }

  struct RawEntry {
    std::string word;
    size_t freq;
  };
  std::vector<RawEntry> entries;
  std::string line;
  size_t total_freq = 0;
  while (std::getline(ifs, line)) {
    if (line.empty()) continue;
    std::stringstream ss(line);
    std::string word;
    size_t freq = 0;
    if (ss >> word >> freq) {
      entries.push_back({word, freq});
      total_freq += freq;
    }
  }
  if (entries.empty()) {
    return false;
  }

  std::sort(entries.begin(), entries.end(), [](const RawEntry &a, const RawEntry &b) {
    return a.word < b.word;
  });

  std::vector<const char *> keys;
  std::vector<size_t> lengths;
  std::vector<Darts::DoubleArray::value_type> values;
  std::vector<float> weights;
  double min_weight = 0.0;

  for (size_t i = 0; i < entries.size(); ++i) {
    if (i > 0 && entries[i].word == entries[i - 1].word) {
      continue;
    }

    double w = std::log(double(entries[i].freq > 0 ? entries[i].freq : 1) / total_freq);
    if (w < min_weight) {
      min_weight = w;
    }

    weights.push_back(static_cast<float>(w));
    keys.push_back(entries[i].word.c_str());
    lengths.push_back(entries[i].word.size());
    values.push_back(static_cast<Darts::DoubleArray::value_type>(weights.size() - 1));
  }

  Darts::DoubleArray da;
  if (da.build(keys.size(), keys.data(), lengths.data(), values.data()) != 0) {
    return false;
  }

  std::string tmp_path = output_path + ".tmp." + std::to_string(getpid()) + "_" +
                         std::to_string(reinterpret_cast<uintptr_t>(&output_path));
  std::ofstream ofs(tmp_path, std::ios::binary);
  if (!ofs.is_open()) {
    return false;
  }

  DatHeader header;
  std::memcpy(header.magic, "DATJIEBA", 8);
  header.version = 1;
  header.num_words = static_cast<uint32_t>(weights.size());
  header.min_weight = min_weight;
  header.da_size = static_cast<uint32_t>(da.total_size());
  header.reserved = 0;

  ofs.write(reinterpret_cast<const char *>(&header), sizeof(header));
  ofs.write(reinterpret_cast<const char *>(da.array()), da.total_size());
  ofs.write(reinterpret_cast<const char *>(weights.data()), weights.size() * sizeof(float));
  ofs.close();

  if (std::rename(tmp_path.c_str(), output_path.c_str()) != 0) {
    std::remove(tmp_path.c_str());
    return false;
  }
  return true;
}

}  // namespace

DatJieba::DatJieba() {
  ResetSeparators(cppjieba::SPECIAL_SEPARATORS);
}

bool DatJieba::ResetSeparators(const std::string &s) {
  symbols_.clear();
  cppjieba::RuneStrArray runes;
  if (!cppjieba::DecodeUTF8RunesInString(s, runes)) {
    return false;
  }
  for (size_t i = 0; i < runes.size(); i++) {
    symbols_.insert(runes[i].rune);
  }
  return true;
}

bool DatJieba::Init(const std::string &dict_dir) {
  is_initialized_ = false;

  std::string bin_path = dict_dir + "/jieba_dat.bin";
  if (!mmap_.Open(bin_path)) {
    // If jieba_dat.bin is missing, auto-compile from jieba.dict.utf8
    std::string text_dict = dict_dir + "/jieba.dict.utf8";
    LOG_INFO("[DatJieba] DAT binary not found at '{}'. Auto-compiling from '{}'...", bin_path, text_dict);
    if (!CompileTextDictToDat(text_dict, bin_path)) {
      LOG_ERROR("[DatJieba] Failed to compile DAT binary from '{}'", text_dict);
      return false;
    }
    if (!mmap_.Open(bin_path)) {
      LOG_ERROR("[DatJieba] Failed to mmap freshly compiled DAT binary: '{}'", bin_path);
      return false;
    }
  }

  if (mmap_.size() < sizeof(DatHeader)) {
    LOG_ERROR("[DatJieba] DAT binary file too small ({} bytes)", mmap_.size());
    mmap_.Close();
    return false;
  }

  const char *ptr = static_cast<const char *>(mmap_.data());
  const auto *header = reinterpret_cast<const DatHeader *>(ptr);
  if (std::memcmp(header->magic, "DATJIEBA", 8) != 0 || header->version != 1) {
    LOG_ERROR("[DatJieba] Invalid DAT binary header magic or unsupported version");
    mmap_.Close();
    return false;
  }

  size_t required_size = sizeof(DatHeader) + static_cast<size_t>(header->da_size) +
                         static_cast<size_t>(header->num_words) * sizeof(float);
  if (mmap_.size() < required_size) {
    LOG_ERROR("[DatJieba] DAT binary file truncated/corrupted: file_size={}, required={}", mmap_.size(),
              required_size);
    mmap_.Close();
    return false;
  }

  min_weight_ = header->min_weight;
  ptr += sizeof(DatHeader);

  da_.set_array(ptr);
  ptr += header->da_size;

  weights_ = reinterpret_cast<const float *>(ptr);

  // Initialize compact HMM model (~50KB)
  std::string hmm_path = dict_dir + "/hmm_model.utf8";
  hmm_model_ = std::make_unique<cppjieba::HMMModel>(hmm_path);
  hmm_seg_ = std::make_unique<cppjieba::HMMSegment>(hmm_model_.get());

  // Optionally load user.dict.utf8 into Overlay dictionary
  std::string user_dict = dict_dir + "/user.dict.utf8";
  LoadUserDict(user_dict);

  is_initialized_ = true;
  LOG_INFO("[DatJieba] Successfully loaded DAT dictionary via mmap ({} words, {:.2f} MB, min_weight={:.2f})",
           header->num_words, mmap_.size() / (1024.0 * 1024.0), min_weight_);
  return true;
}

void DatJieba::LoadUserDict(const std::string &user_dict_path) {
  std::ifstream ifs(user_dict_path);
  if (!ifs.is_open()) return;

  std::string line;
  size_t count = 0;
  while (std::getline(ifs, line)) {
    if (line.empty()) continue;
    std::stringstream ss(line);
    std::string word;
    int freq = 0;
    std::string tag;
    if (ss >> word) {
      ss >> freq >> tag;
      if (InsertUserWord(word, freq, tag)) {
        count++;
      }
    }
  }
  if (count > 0) {
    LOG_INFO("[DatJieba] Loaded {} user-defined words into overlay dictionary from: '{}'", count, user_dict_path);
  }
}

bool DatJieba::InsertUserWord(const std::string &word, int freq, const std::string &tag) {
  if (word.empty()) return false;

  std::lock_guard<std::mutex> lock(user_dict_mutex_);

  // High default weight so user custom words are prioritized in shortest path DP
  double weight = (freq > 0) ? std::log(double(freq) / 1000000.0) : 0.0;
  UserWordInfo uw{word, weight, tag};
  user_words_[word] = uw;

  cppjieba::RuneStrArray runes;
  if (cppjieba::DecodeUTF8RunesInString(word, runes) && runes.size() == 1) {
    if (runes[0].rune >= 0x80) {
      user_single_cjk_.insert(runes[0].rune);
    }
  }
  return true;
}

bool DatJieba::DeleteUserWord(const std::string &word) {
  std::lock_guard<std::mutex> lock(user_dict_mutex_);
  auto it = user_words_.find(word);
  if (it != user_words_.end()) {
    user_words_.erase(it);
    return true;
  }
  return false;
}

void DatJieba::Cut(const std::string &sentence, std::vector<cppjieba::Word> &words, bool hmm) const {
  words.clear();
  if (!is_initialized_ || sentence.empty()) return;

  std::string valid_sentence;
  const std::string *text_ptr = &sentence;

  if (!utf8::is_valid(sentence.begin(), sentence.end())) {
    utf8::replace_invalid(sentence.begin(), sentence.end(), std::back_inserter(valid_sentence));
    text_ptr = &valid_sentence;
  }

  cppjieba::PreFilter pre_filter(symbols_, *text_ptr);
  std::vector<cppjieba::WordRange> wrs;
  wrs.reserve(text_ptr->size() / 2);

  while (pre_filter.HasNext()) {
    auto range = pre_filter.Next();
    CutRange(*text_ptr, range.begin, range.end, wrs, hmm);
  }

  cppjieba::GetWordsFromWordRanges(*text_ptr, wrs, words);
}

void DatJieba::Cut(const std::string &sentence, std::vector<std::string> &words, bool hmm) const {
  std::vector<cppjieba::Word> tmp;
  Cut(sentence, tmp, hmm);
  words.clear();
  words.reserve(tmp.size());
  for (const auto &w : tmp) {
    words.push_back(w.word);
  }
}

void DatJieba::CutRange(const std::string &sentence, cppjieba::RuneStrArray::const_iterator begin,
                        cppjieba::RuneStrArray::const_iterator end, std::vector<cppjieba::WordRange> &res,
                        bool hmm) const {
  if (begin >= end) return;

  size_t num_runes = end - begin;
  std::vector<DagNode> dag(num_runes);

  Darts::DoubleArray::result_pair_type results[64];

  std::lock_guard<std::mutex> lock(user_dict_mutex_);

  // Build DAG
  for (size_t i = 0; i < num_runes; ++i) {
    uint32_t curr_byte_offset = begin[i].offset;
    const char *p_curr = sentence.data() + curr_byte_offset;
    uint32_t end_byte_offset = (end - 1)->offset + (end - 1)->len;
    size_t max_bytes = end_byte_offset - curr_byte_offset;

    // 1. Search in Darts DoubleArray
    size_t num_matches = da_.commonPrefixSearch(p_curr, results, 64, max_bytes);
    for (size_t m = 0; m < num_matches; ++m) {
      size_t match_bytes = results[m].length;
      float w = weights_[results[m].value];

      // Map byte length back to rune index
      size_t j = i;
      while (j < num_runes) {
        uint32_t r_end = begin[j].offset + begin[j].len;
        if (static_cast<size_t>(r_end - curr_byte_offset) == match_bytes) {
          dag[i].edges.push_back({j, w});
          break;
        }
        if (static_cast<size_t>(r_end - curr_byte_offset) > match_bytes) {
          break;
        }
        j++;
      }
    }

    // 2. Search in Dynamic User Dict (Overlay)
    if (!user_words_.empty()) {
      std::string cand;
      cand.reserve(32);
      for (size_t j = i; j < num_runes && (j - i) < 20; ++j) {
        cand.append(sentence.data() + begin[j].offset, begin[j].len);
        auto it = user_words_.find(cand);
        if (it != user_words_.end()) {
          dag[i].edges.push_back({j, it->second.weight});
        }
      }
    }

    // 3. Fallback single-character edge
    bool has_single = false;
    for (const auto &e : dag[i].edges) {
      if (e.next_rune == i) {
        has_single = true;
        break;
      }
    }
    if (!has_single) {
      dag[i].edges.push_back({i, min_weight_});
    }
  }

  // Dynamic Programming (Backward Viterbi)
  for (int i = static_cast<int>(num_runes) - 1; i >= 0; --i) {
    dag[i].best_weight = -1e20;
    dag[i].best_next_rune = i;

    for (const auto &edge : dag[i].edges) {
      double val = edge.weight;
      if (edge.next_rune + 1 < num_runes) {
        val += dag[edge.next_rune + 1].best_weight;
      }
      if (val > dag[i].best_weight) {
        dag[i].best_weight = val;
        dag[i].best_next_rune = edge.next_rune;
      }
    }
  }

  // Forward DAG path extraction (MP Segment)
  std::vector<cppjieba::WordRange> mp_words;
  size_t curr = 0;
  while (curr < num_runes) {
    size_t next = dag[curr].best_next_rune;
    mp_words.emplace_back(begin + curr, begin + next);
    curr = next + 1;
  }

  if (!hmm) {
    res.insert(res.end(), mp_words.begin(), mp_words.end());
    return;
  }

  // MixSegment HMM combination
  std::vector<cppjieba::WordRange> hmm_res;
  for (size_t i = 0; i < mp_words.size(); ++i) {
    if (mp_words[i].left != mp_words[i].right ||
        (mp_words[i].left == mp_words[i].right && user_single_cjk_.count(mp_words[i].left->rune))) {
      res.push_back(mp_words[i]);
      continue;
    }

    // Consecutive single-character run without user dictionary entry
    size_t j = i;
    while (j < mp_words.size() && mp_words[j].left == mp_words[j].right &&
           !user_single_cjk_.count(mp_words[j].left->rune)) {
      j++;
    }

    hmm_res.clear();
    hmm_seg_->Cut(mp_words[i].left, mp_words[j - 1].left + 1, hmm_res);
    res.insert(res.end(), hmm_res.begin(), hmm_res.end());

    i = j - 1;
  }
}

}  // namespace storage
}  // namespace soeasy
