// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "service/dictionary_service.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

#include "common/encoding_utils.h"
#include "storage/dat_jieba.h"

namespace soeasy {
namespace service {

namespace {

std::string TrimAsciiWhitespace(const std::string &input) {
  size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
    ++start;
  }
  size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }
  return input.substr(start, end - start);
}

}  // namespace

struct DictionaryService::Impl {
  std::unique_ptr<storage::DatJieba> jieba;
  bool initialized = false;
};

DictionaryService::DictionaryService() : impl_(std::make_unique<Impl>()) {}
DictionaryService::~DictionaryService() = default;
DictionaryService::DictionaryService(DictionaryService &&) noexcept = default;
DictionaryService &DictionaryService::operator=(DictionaryService &&) noexcept = default;

bool DictionaryService::InitBaseDictionary(const std::vector<std::string> &candidate_dict_dirs) {
  if (!impl_) {
    return false;
  }
  if (impl_->initialized) {
    return true;
  }
  if (!impl_->jieba) {
    impl_->jieba = std::make_unique<storage::DatJieba>();
  }
  for (const auto &dir : candidate_dict_dirs) {
    if (!dir.empty() && impl_->jieba->Init(dir)) {
      impl_->initialized = true;
      return true;
    }
  }
  return false;
}

bool DictionaryService::IsInBaseDictionary(const std::string &word) const {
  if (!impl_ || !impl_->jieba || !impl_->initialized || word.empty()) {
    return false;
  }
  return impl_->jieba->HasWord(word);
}

std::vector<std::string> DictionaryService::LoadUserWords(const std::string &user_dict_path) const {
  std::vector<std::string> words;
  if (user_dict_path.empty()) {
    return words;
  }
  auto in = common::EncodingUtils::OpenIfstream(user_dict_path, std::ios::in);
  if (!in.is_open()) {
    return words;
  }
  std::string line;
  while (std::getline(in, line)) {
    const std::string trimmed = TrimAsciiWhitespace(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    std::istringstream iss(trimmed);
    std::string word;
    if (iss >> word && !word.empty()) {
      words.push_back(std::move(word));
    }
  }
  return words;
}

bool DictionaryService::SaveUserWords(const std::string &user_dict_path, const std::vector<std::string> &words) const {
  if (user_dict_path.empty()) {
    return false;
  }
  std::error_code ec;
  const auto path = common::EncodingUtils::PathFromUtf8(user_dict_path);
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  auto out = common::EncodingUtils::OpenOfstream(user_dict_path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  for (const auto &word : words) {
    const std::string trimmed = TrimAsciiWhitespace(word);
    if (!trimmed.empty()) {
      out << trimmed << "\n";
    }
  }
  return out.good();
}

}  // namespace service
}  // namespace soeasy
