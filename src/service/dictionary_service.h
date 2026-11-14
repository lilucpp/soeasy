// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_SERVICE_DICTIONARY_SERVICE_H_
#define SOEASY_SRC_SERVICE_DICTIONARY_SERVICE_H_

#include <memory>
#include <string>
#include <vector>

namespace soeasy {
namespace service {

/**
 * @brief Service-layer facade for dictionary lookup and user dictionary persistence.
 *
 * Isolates storage::DatJieba behind a PIMPL boundary so GUI components never depend
 * on storage headers or concrete dictionary implementations.
 */
class DictionaryService {
 public:
  DictionaryService();
  ~DictionaryService();

  DictionaryService(const DictionaryService &) = delete;
  DictionaryService &operator=(const DictionaryService &) = delete;
  DictionaryService(DictionaryService &&) noexcept;
  DictionaryService &operator=(DictionaryService &&) noexcept;

  /**
   * @brief Initialize base dictionary from the first valid directory in candidates.
   */
  bool InitBaseDictionary(const std::vector<std::string> &candidate_dict_dirs);

  /**
   * @brief Check whether a word already exists in the base dictionary.
   */
  [[nodiscard]] bool IsInBaseDictionary(const std::string &word) const;

  /**
   * @brief Load custom user words from UTF-8 user dictionary file.
   */
  [[nodiscard]] std::vector<std::string> LoadUserWords(const std::string &user_dict_path) const;

  /**
   * @brief Persist custom user words to UTF-8 user dictionary file (creating parent dirs).
   */
  bool SaveUserWords(const std::string &user_dict_path, const std::vector<std::string> &words) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_DICTIONARY_SERVICE_H_
