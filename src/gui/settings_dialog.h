#ifndef SOEASY_SRC_GUI_SETTINGS_DIALOG_H_
#define SOEASY_SRC_GUI_SETTINGS_DIALOG_H_

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QSpinBox>
#include <QTabWidget>

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
  void OnAddExtension();
  void OnRemoveExtension();
  void OnResetExtensionsToDefault();
  void OnSaveAndApply();
  void OnRebuildIndex();

 private:
  void SetupUi();
  void PopulateFromConfig();

  std::shared_ptr<service::IndexConfig> config_;

  QTabWidget* tab_widget_ = nullptr;

  // Tab 1: Indexed directories
  QListWidget* dirs_list_ = nullptr;

  // Tab 2: Excluded patterns
  QListWidget* patterns_list_ = nullptr;
  QLineEdit* new_pattern_edit_ = nullptr;

  // Tab 3: Custom file extensions
  QListWidget* exts_list_ = nullptr;
  QLineEdit* new_ext_edit_ = nullptr;

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
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_DIALOG_H_
