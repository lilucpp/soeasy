#ifndef SOEASY_SRC_GUI_SETTINGS_DIALOG_H_
#define SOEASY_SRC_GUI_SETTINGS_DIALOG_H_

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <memory>
#include <string>

#include "service/index_config.h"

namespace soeasy {
namespace gui {

class SettingsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit SettingsDialog(std::shared_ptr<service::IndexConfig> config,
                          QWidget* parent = nullptr);
  ~SettingsDialog() override = default;

 signals:
  void SettingsApplied();
  void RebuildIndexRequested();

 private slots:
  void OnAddDirectory();
  void OnRemoveDirectory();
  void OnAddPattern();
  void OnRemovePattern();
  void OnAddCustomExtension();
  void OnRemoveCustomExtension();
  void OnSelectAllExtensions();
  void OnDeselectAllExtensions();
  void OnResetExtensionsToDefault();
  void OnTreeItemChanged(QTreeWidgetItem* item, int column);
  void OnBrowseDocToTextLogPath();
  void OnSaveAndApply();
  void OnRebuildIndex();

 private:
  void SetupUi();
  void PopulateFromConfig();
  void UpdateExtensionsCount();

  std::shared_ptr<service::IndexConfig> config_;

  QTabWidget* tab_widget_ = nullptr;

  // Tab 1: Indexed directories
  QListWidget* dirs_list_ = nullptr;

  // Tab 2: Excluded patterns
  QListWidget* patterns_list_ = nullptr;
  QLineEdit* new_pattern_edit_ = nullptr;

  // Tab 3: File categories & Custom extensions
  QCheckBox* embedded_image_ocr_check_ = nullptr;
  QTreeWidget* exts_tree_ = nullptr;
  QTreeWidgetItem* custom_category_item_ = nullptr;
  QLineEdit* new_custom_ext_edit_ = nullptr;
  QPushButton* btn_add_custom_ext_ = nullptr;
  QPushButton* btn_rm_custom_ext_ = nullptr;
  QLabel* exts_count_label_ = nullptr;
  bool is_updating_tree_ = false;

  // Tab 4: Performance & Scheduling
  QSpinBox* debounce_spin_ = nullptr;
  QSpinBox* max_size_spin_ = nullptr;
  QSpinBox* worker_count_spin_ = nullptr;
  QSpinBox* min_img_chars_spin_ = nullptr;
  QCheckBox* filter_thumbs_check_ = nullptr;

  // Logging & Debugging options
  QComboBox* log_level_combo_ = nullptr;
  QCheckBox* log_console_check_ = nullptr;
  QCheckBox* log_sync_check_ = nullptr;
  QCheckBox* doctotext_log_check_ = nullptr;
  QLineEdit* doctotext_log_path_edit_ = nullptr;
  QPushButton* dt_browse_btn_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_DIALOG_H_
