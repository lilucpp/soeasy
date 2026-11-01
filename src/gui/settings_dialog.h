#ifndef SOEASY_SRC_GUI_SETTINGS_DIALOG_H_
#define SOEASY_SRC_GUI_SETTINGS_DIALOG_H_

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QGroupBox>
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
  void RetranslateUi();
  void PopulateFromConfig();
  void UpdateExtensionsCount();

  std::shared_ptr<service::IndexConfig> config_;

  QTabWidget* tab_widget_ = nullptr;

  // Tab 0: General & Language
  QLabel* label_language_ = nullptr;
  QComboBox* language_combo_ = nullptr;
  QLabel* label_lang_hint_ = nullptr;

  // Tab 1: Indexed directories
  QLabel* label_dirs_ = nullptr;
  QListWidget* dirs_list_ = nullptr;
  QPushButton* btn_add_dir_ = nullptr;
  QPushButton* btn_rm_dir_ = nullptr;

  // Tab 2: Excluded patterns
  QLabel* label_patterns_ = nullptr;
  QListWidget* patterns_list_ = nullptr;
  QLineEdit* new_pattern_edit_ = nullptr;
  QPushButton* btn_add_pattern_ = nullptr;
  QPushButton* btn_rm_pattern_ = nullptr;

  // Tab 3: File categories & Custom extensions
  QGroupBox* group_ocr_ = nullptr;
  QCheckBox* embedded_image_ocr_check_ = nullptr;
  QLabel* label_ocr_tip_ = nullptr;
  QLabel* label_tree_title_ = nullptr;
  QTreeWidget* exts_tree_ = nullptr;
  QTreeWidgetItem* custom_category_item_ = nullptr;
  QLabel* label_custom_ = nullptr;
  QLineEdit* new_custom_ext_edit_ = nullptr;
  QPushButton* btn_add_custom_ext_ = nullptr;
  QPushButton* btn_rm_custom_ext_ = nullptr;
  QPushButton* btn_select_all_ = nullptr;
  QPushButton* btn_deselect_all_ = nullptr;
  QPushButton* btn_reset_exts_ = nullptr;
  QLabel* exts_count_label_ = nullptr;
  bool is_updating_tree_ = false;

  // Tab 4: Performance & Scheduling
  QLabel* label_debounce_ = nullptr;
  QSpinBox* debounce_spin_ = nullptr;
  QLabel* label_max_size_ = nullptr;
  QSpinBox* max_size_spin_ = nullptr;
  QLabel* label_worker_count_ = nullptr;
  QSpinBox* worker_count_spin_ = nullptr;
  QLabel* label_min_img_chars_ = nullptr;
  QSpinBox* min_img_chars_spin_ = nullptr;
  QCheckBox* filter_thumbs_check_ = nullptr;

  // Logging & Debugging options
  QLabel* label_log_level_ = nullptr;
  QComboBox* log_level_combo_ = nullptr;
  QCheckBox* log_console_check_ = nullptr;
  QCheckBox* log_sync_check_ = nullptr;
  QCheckBox* doctotext_log_check_ = nullptr;
  QLabel* label_dt_path_ = nullptr;
  QLineEdit* doctotext_log_path_edit_ = nullptr;
  QPushButton* dt_browse_btn_ = nullptr;

  // Dialog actions
  QPushButton* btn_rebuild_ = nullptr;
  QPushButton* btn_save_ = nullptr;
  QPushButton* btn_cancel_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_DIALOG_H_
