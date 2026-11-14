// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_SETTINGS_EXTENSIONS_TAB_H_
#define SOEASY_SRC_GUI_SETTINGS_EXTENSIONS_TAB_H_

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "gui/settings/isettings_tab.h"

namespace soeasy {
namespace gui {

class ExtensionsTab : public ISettingsTab {
  Q_OBJECT

 public:
  explicit ExtensionsTab(QWidget *parent = nullptr);
  ~ExtensionsTab() override = default;

  void PopulateFromConfig(const service::IndexConfig &config) override;
  void ApplyToConfig(service::IndexConfig *config) override;
  void RetranslateUi() override;
  void SetStructuralReadOnly(bool read_only) override;

 private slots:
  void OnAddCustomExtension();
  void OnRemoveCustomExtension();
  void OnSelectAllExtensions();
  void OnDeselectAllExtensions();
  void OnResetExtensionsToDefault();
  void OnTreeItemChanged(QTreeWidgetItem *item, int column);

 private:
  void SetupUi();
  void UpdateExtensionsCount();

  QGroupBox *group_ocr_ = nullptr;
  QCheckBox *embedded_image_ocr_check_ = nullptr;
  QLabel *label_ocr_tip_ = nullptr;
  QLabel *label_tree_title_ = nullptr;
  QTreeWidget *exts_tree_ = nullptr;
  QTreeWidgetItem *custom_category_item_ = nullptr;
  QLabel *label_custom_ = nullptr;
  QLineEdit *new_custom_ext_edit_ = nullptr;
  QPushButton *btn_add_custom_ext_ = nullptr;
  QPushButton *btn_rm_custom_ext_ = nullptr;
  QPushButton *btn_select_all_ = nullptr;
  QPushButton *btn_deselect_all_ = nullptr;
  QPushButton *btn_reset_exts_ = nullptr;
  QLabel *exts_count_label_ = nullptr;

  bool is_updating_tree_ = false;
  bool structural_read_only_ = false;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_EXTENSIONS_TAB_H_
