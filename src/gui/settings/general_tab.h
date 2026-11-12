// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_SETTINGS_GENERAL_TAB_H_
#define SOEASY_SRC_GUI_SETTINGS_GENERAL_TAB_H_

#include <QComboBox>
#include <QLabel>

#include "gui/settings/isettings_tab.h"

namespace soeasy {
namespace gui {

class GeneralTab : public ISettingsTab {
  Q_OBJECT

 public:
  explicit GeneralTab(QWidget *parent = nullptr);
  ~GeneralTab() override = default;

  void PopulateFromConfig(const service::IndexConfig &config) override;
  void ApplyToConfig(service::IndexConfig *config) override;
  void RetranslateUi() override;

 signals:
  void LanguageChanged(const std::string &lang);

 private:
  void SetupUi();

  QLabel *label_language_ = nullptr;
  QComboBox *language_combo_ = nullptr;
  QLabel *label_lang_hint_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_GENERAL_TAB_H_
