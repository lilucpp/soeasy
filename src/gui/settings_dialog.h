// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_SETTINGS_DIALOG_H_
#define SOEASY_SRC_GUI_SETTINGS_DIALOG_H_

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <functional>
#include <memory>
#include <vector>

#include "gui/settings/isettings_tab.h"
#include "service/index_config.h"

namespace soeasy {
namespace gui {

/**
 * @brief Main settings dialog container that coordinates modular setting pages.
 */
class SettingsDialog : public QDialog {
  Q_OBJECT

 public:
  struct IndexingControl {
    std::function<bool()> is_indexing;
    std::function<void()> cancel_indexing;
  };

  explicit SettingsDialog(std::shared_ptr<service::IndexConfig> config, QWidget *parent = nullptr,
                          IndexingControl indexing_control = {});
  ~SettingsDialog() override = default;

  void SetCurrentTab(int index);

 signals:
  void SettingsApplied();
  void SettingsAppliedWithDiff(const soeasy::service::ConfigChangeSummary &diff);
  void RebuildIndexRequested();

 private slots:
  void OnSaveAndApply();
  void OnRebuildIndex();
  void OnStopIndexingClicked();
  void OnLanguageChanged(const std::string &lang);
  void UpdateIndexingLockState();

 private:
  void SetupUi();
  void RetranslateUi();
  void PopulateFromConfig();
  QHBoxLayout *CreateBottomBar();

  std::shared_ptr<service::IndexConfig> config_;
  IndexingControl indexing_control_;
  bool was_structural_locked_ = false;

  QWidget *lock_banner_ = nullptr;
  QLabel *lock_banner_label_ = nullptr;
  QPushButton *btn_stop_indexing_ = nullptr;
  QTimer *lock_poll_timer_ = nullptr;

  QListWidget *nav_list_ = nullptr;
  QStackedWidget *page_stack_ = nullptr;
  std::vector<const char *> nav_title_keys_;
  std::vector<ISettingsTab *> tabs_;

  QPushButton *btn_rebuild_ = nullptr;
  QPushButton *btn_save_ = nullptr;
  QPushButton *btn_cancel_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_DIALOG_H_
