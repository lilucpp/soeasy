// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "darts.h"

#pragma pack(push, 1)
struct DatHeader {
  char magic[8];       // "DATJIEBA"
  uint32_t version;    // 1
  uint32_t num_words;  // count of words
  double min_weight;   // min log weight
  uint32_t da_size;    // bytes of Darts array
  uint32_t reserved;   // padding for 8-byte alignment
};
#pragma pack(pop)

struct WordEntry {
  std::string word;
  size_t freq;
};

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <input_jieba_dict_utf8> <output_dat_bin>" << std::endl;
    return 1;
  }

  std::string input_path = argv[1];
  std::string output_path = argv[2];

  std::cout << "[BuildDatDict] Loading text dictionary: " << input_path << "..." << std::endl;
  std::ifstream ifs(input_path);
  if (!ifs.is_open()) {
    std::cerr << "[-] Error: Failed to open input dictionary: " << input_path << std::endl;
    return 1;
  }

  std::vector<WordEntry> entries;
  std::string line;
  size_t total_freq = 0;

  auto t0 = std::chrono::high_resolution_clock::now();
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
  ifs.close();

  if (entries.empty()) {
    std::cerr << "[-] Error: Dictionary file is empty!" << std::endl;
    return 1;
  }

  std::cout << "[BuildDatDict] Loaded " << entries.size() << " raw entries. Sorting lexicographically..." << std::endl;
  std::sort(entries.begin(), entries.end(), [](const WordEntry &a, const WordEntry &b) {
    return a.word < b.word;
  });

  std::vector<const char *> keys;
  std::vector<size_t> lengths;
  std::vector<Darts::DoubleArray::value_type> values;
  std::vector<float> weights;
  double min_weight = 0.0;

  for (size_t i = 0; i < entries.size(); ++i) {
    // Deduplicate identical words
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

  std::cout << "[BuildDatDict] Building Double-Array Trie (Darts) for " << keys.size() << " unique words..."
            << std::endl;
  Darts::DoubleArray da;
  if (da.build(keys.size(), keys.data(), lengths.data(), values.data()) != 0) {
    std::cerr << "[-] Error: Failed to build Darts DoubleArray!" << std::endl;
    return 1;
  }

  std::ofstream ofs(output_path, std::ios::binary);
  if (!ofs.is_open()) {
    std::cerr << "[-] Error: Failed to open output file: " << output_path << std::endl;
    return 1;
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

  auto t1 = std::chrono::high_resolution_clock::now();
  double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  std::cout << "[BuildDatDict] SUCCESS: Generated " << output_path << " (" << (da.total_size() + weights.size() * sizeof(float)) / (1024.0 * 1024.0)
            << " MB) in " << elapsed_ms << " ms!" << std::endl;
  return 0;
}
