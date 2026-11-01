#ifndef SOEASY_SRC_GUI_I18N_H_
#define SOEASY_SRC_GUI_I18N_H_

#include <QString>
#include <string>
#include <unordered_map>

namespace soeasy {
namespace gui {

enum class Language {
  kChinese,
  kEnglish,
};

class I18n {
 public:
  static I18n& Instance();

  void SetLanguage(const std::string& lang_code);
  std::string GetLanguageCode() const;
  Language current_language() const { return current_lang_; }

  QString Get(const char* key) const;

  QString GetCategoryName(size_t cat_index, const QString& default_name) const;
  QString GetCategoryDesc(size_t cat_index, const QString& default_desc) const;
  QString GetFormatDesc(const QString& ext, const QString& default_desc) const;

 private:
  I18n();
  void InitDictionary();

  Language current_lang_ = Language::kChinese;
  std::unordered_map<std::string, std::string> zh_dict_;
  std::unordered_map<std::string, std::string> en_dict_;
};

/**
 * @brief Global helper function for UI text translation.
 */
inline QString Tr(const char* key) {
  return I18n::Instance().Get(key);
}

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_I18N_H_
