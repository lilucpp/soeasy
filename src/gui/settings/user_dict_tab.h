// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_SETTINGS_USER_DICT_TAB_H_
#define SOEASY_SRC_GUI_SETTINGS_USER_DICT_TAB_H_

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <memory>

#include "gui/settings/isettings_tab.h"
#include "service/dictionary_service.h"

namespace soeasy {
namespace gui {

class UserDictTab : public ISettingsTab {
  Q_OBJECT

 public:
  explicit UserDictTab(QWidget *parent = nullptr);
  ~UserDictTab() override = default;

  void PopulateFromConfig(const service::IndexConfig &config) override;
  void ApplyToConfig(service::IndexConfig *config) override;
  void RetranslateUi() override;

 private slots:
  void OnAddUserWord();
  void OnRemoveUserWord();

 private:
  void SetupUi();
  void LoadUserDict();
  void SaveUserDict();

  QLabel *label_user_dict_hint_ = nullptr;
  QTableWidget *user_dict_table_ = nullptr;
  QLineEdit *user_word_input_ = nullptr;
  QPushButton *btn_add_user_word_ = nullptr;
  QPushButton *btn_rm_user_word_ = nullptr;
  QLabel *user_dict_count_label_ = nullptr;

  std::unique_ptr<service::DictionaryService> dictionary_service_;
  QString user_dict_file_path_;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_USER_DICT_TAB_H_
