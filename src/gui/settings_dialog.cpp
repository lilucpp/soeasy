// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings_dialog.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>

#include "gui/i18n.h"
#include "gui/settings/extensions_tab.h"
#include "gui/settings/general_tab.h"
#include "gui/settings/performance_tab.h"

namespace soeasy {
namespace gui {

SettingsDialog::SettingsDialog(std::shared_ptr<service::IndexConfig> config, QWidget *parent,
                               IndexingControl indexing_control)
    : QDialog(parent), config_(std::move(config)), indexing_control_(std::move(indexing_control)) {
  SetupUi();
  PopulateFromConfig();
  UpdateIndexingLockState();
  if (indexing_control_.is_indexing) {
    lock_poll_timer_ = new QTimer(this);
    connect(lock_poll_timer_, &QTimer::timeout, this, &SettingsDialog::UpdateIndexingLockState);
    lock_poll_timer_->start(120);
  }
}

void SettingsDialog::SetupUi() {
  resize(860, 600);
  setStyleSheet(
      "QDialog {"
      "  background-color: #FFFFFF;"
      "  color: #1E293B;"
      "  font-family: 'Microsoft YaHei UI', 'Microsoft YaHei', 'Segoe UI', 'PingFang SC', 'Noto Sans CJK SC', "
      "'WenQuanYi Micro Hei', sans-serif;"
      "}"
      "QListWidget#settings_nav_list {"
      "  background-color: #F8FAFC;"
      "  border: none;"
      "  border-right: 1px solid #E2E8F0;"
      "  outline: none;"
      "  padding: 12px 8px;"
      "}"
      "QListWidget#settings_nav_list::item {"
      "  min-height: 40px;"
      "  max-height: 44px;"
      "  border-radius: 6px;"
      "  color: #334155;"
      "  font-size: 13px;"
      "  font-weight: 500;"
      "  padding-left: 14px;"
      "  margin-bottom: 6px;"
      "}"
      "QListWidget#settings_nav_list::item:hover {"
      "  background-color: #F1F5F9;"
      "  color: #0F172A;"
      "}"
      "QListWidget#settings_nav_list::item:selected {"
      "  background-color: #E0F2FE;"
      "  color: #0284C7;"
      "  font-weight: 600;"
      "}"
      "QStackedWidget#settings_page_stack {"
      "  background-color: #FFFFFF;"
      "  border: none;"
      "}"
      "QWidget#settings_bottom_container {"
      "  background-color: #F8FAFC;"
      "  border-top: 1px solid #E2E8F0;"
      "  padding: 8px 16px;"
      "}"
      "QTreeWidget, QListWidget, QTableWidget {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: 4px 6px;"
      "  color: #1E293B;"
      "}"
      "QLineEdit {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: 4px 8px;"
      "  color: #1E293B;"
      "  min-height: 24px;"
      "}"
      "QLineEdit:hover { border-color: #94A3B8; }"
      "QLineEdit:focus { border-color: #0284C7; }"
      "QComboBox {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: 4px 28px 4px 10px;"
      "  color: #1E293B;"
      "  min-height: 24px;"
      "}"
      "QComboBox:hover { border-color: #94A3B8; }"
      "QComboBox:focus, QComboBox:on { border-color: #0284C7; }"
      "QComboBox::drop-down {"
      "  subcontrol-origin: padding;"
      "  subcontrol-position: top right;"
      "  width: 26px;"
      "  border: none;"
      "  background: transparent;"
      "}"
      "QComboBox::down-arrow {"
      "  image: url(:/icons/chevron_down.png);"
      "  width: 12px;"
      "  height: 12px;"
      "}"
      "QComboBox QAbstractItemView {"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  selection-background-color: #E0F2FE;"
      "  selection-color: #0284C7;"
      "  padding: 4px;"
      "  outline: none;"
      "}"
      "QSpinBox {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: 4px 24px 4px 10px;"
      "  color: #1E293B;"
      "  min-height: 24px;"
      "}"
      "QSpinBox:hover { border-color: #94A3B8; }"
      "QSpinBox:focus { border-color: #0284C7; }"
      "QSpinBox::up-button, QSpinBox::down-button {"
      "  width: 20px;"
      "  border: none;"
      "  background: #F1F5F9;"
      "}"
      "QSpinBox::up-button { subcontrol-position: top right; border-top-right-radius: 5px; }"
      "QSpinBox::down-button { subcontrol-position: bottom right; border-bottom-right-radius: 5px; }"
      "QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #E2E8F0; }"
      "QSpinBox::up-arrow {"
      "  image: url(:/icons/chevron_up.png);"
      "  width: 10px;"
      "  height: 10px;"
      "}"
      "QSpinBox::down-arrow {"
      "  image: url(:/icons/chevron_down.png);"
      "  width: 10px;"
      "  height: 10px;"
      "}"
      "QHeaderView::section {"
      "  background-color: #F8FAFC;"
      "  color: #475569;"
      "  font-weight: 600;"
      "  font-size: 12px;"
      "  border: none;"
      "  border-bottom: 1px solid #E2E8F0;"
      "  padding: 5px 8px;"
      "}"
      "QPushButton {"
      "  background-color: #F8FAFC;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  padding: 6px 14px;"
      "  color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }"
      "QPushButton:default {"
      "  background-color: #0284C7;"
      "  border-color: #0369A1;"
      "  color: #FFFFFF;"
      "}"
      "QPushButton:default:hover { background-color: #0369A1; }"
      "QGroupBox {"
      "  border: 1px solid #E2E8F0;"
      "  border-radius: 6px;"
      "  margin-top: 10px;"
      "  padding: 12px 10px 8px 10px;"
      "  font-weight: 600;"
      "  color: #334155;"
      "}"
      "QGroupBox::title {"
      "  subcontrol-origin: margin;"
      "  left: 10px;"
      "  padding: 0 6px;"
      "  background-color: #FFFFFF;"
      "}");

  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  lock_banner_ = new QWidget(this);
  lock_banner_->setObjectName("settings_lock_banner");
  lock_banner_->setStyleSheet(
      "QWidget#settings_lock_banner { background-color: #FEF3C7; border-bottom: 1px solid #F59E0B; }");
  auto *banner_layout = new QHBoxLayout(lock_banner_);
  banner_layout->setContentsMargins(16, 8, 16, 8);
  banner_layout->setSpacing(12);

  lock_banner_label_ = new QLabel(lock_banner_);
  lock_banner_label_->setWordWrap(true);
  lock_banner_label_->setStyleSheet("color: #92400E; font-size: 12px; font-weight: 500; background: transparent;");

  btn_stop_indexing_ = new QPushButton(lock_banner_);
  btn_stop_indexing_->setObjectName("btn_stop_indexing");
  btn_stop_indexing_->setStyleSheet(
      "QPushButton { background-color: #DC2626; color: #FFFFFF; border: none; border-radius: 4px; "
      "padding: 5px 12px; font-weight: 600; }"
      "QPushButton:hover { background-color: #B91C1C; }"
      "QPushButton:disabled { background-color: #FCA5A5; }");
  connect(btn_stop_indexing_, &QPushButton::clicked, this, &SettingsDialog::OnStopIndexingClicked);

  banner_layout->addWidget(lock_banner_label_, 1);
  banner_layout->addWidget(btn_stop_indexing_, 0, Qt::AlignVCenter);
  lock_banner_->setVisible(false);
  main_layout->addWidget(lock_banner_);

  auto *body_widget = new QWidget(this);
  auto *body_layout = new QHBoxLayout(body_widget);
  body_layout->setContentsMargins(0, 0, 0, 0);
  body_layout->setSpacing(0);

  nav_list_ = new QListWidget(body_widget);
  nav_list_->setObjectName("settings_nav_list");
  nav_list_->setFixedWidth(214);
  nav_list_->setFocusPolicy(Qt::NoFocus);
  nav_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  nav_list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  page_stack_ = new QStackedWidget(body_widget);
  page_stack_->setObjectName("settings_page_stack");

  nav_title_keys_.clear();
  tabs_.clear();
  auto register_tab = [this](const char *title_key, ISettingsTab *tab) {
    auto *item = new QListWidgetItem(Tr(title_key), nav_list_);
    item->setSizeHint(QSize(198, 42));
    item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    nav_title_keys_.push_back(title_key);
    tabs_.push_back(tab);
    page_stack_->addWidget(tab);
  };

  // Consolidated 3-tab structure:
  // 0: General & Indexing Scope (Language + Directories + Exclusions + Thumbnail cache filter)
  // 1: File Formats & Parsing (Embedded OCR + Extension tree)
  // 2: Performance & Diagnostics (Resource tuning + Logging)
  // Note: UserDictTab is kept in codebase for reuse; omitted from UI.
  auto *general_tab = new GeneralTab(page_stack_);
  connect(general_tab, &GeneralTab::LanguageChanged, this, &SettingsDialog::OnLanguageChanged);
  register_tab("tab_general", general_tab);
  register_tab("tab_exts", new ExtensionsTab(page_stack_));
  register_tab("tab_perf", new PerformanceTab(page_stack_));

  body_layout->addWidget(nav_list_);
  body_layout->addWidget(page_stack_, 1);
  main_layout->addWidget(body_widget, 1);

  auto *bottom_container = new QWidget(this);
  bottom_container->setObjectName("settings_bottom_container");
  auto *bottom_bar = CreateBottomBar();
  bottom_container->setLayout(bottom_bar);
  main_layout->addWidget(bottom_container);

  connect(nav_list_, &QListWidget::currentRowChanged, page_stack_, &QStackedWidget::setCurrentIndex);
  nav_list_->setCurrentRow(0);

  RetranslateUi();
}

void SettingsDialog::SetCurrentTab(int index) {
  if (nav_list_ && index >= 0 && index < nav_list_->count()) {
    nav_list_->setCurrentRow(index);
  }
}

QHBoxLayout *SettingsDialog::CreateBottomBar() {
  auto *bottom_layout = new QHBoxLayout();
  btn_rebuild_ = new QPushButton(this);
  btn_rebuild_->setObjectName("btn_rebuild");
  btn_rebuild_->setStyleSheet("QPushButton { color: #D32F2F; }");
  bottom_layout->addWidget(btn_rebuild_);
  bottom_layout->addStretch();
  btn_save_ = new QPushButton(this);
  btn_save_->setObjectName("btn_save");
  btn_save_->setDefault(true);
  btn_cancel_ = new QPushButton(this);
  bottom_layout->addWidget(btn_save_);
  bottom_layout->addWidget(btn_cancel_);

  connect(btn_rebuild_, &QPushButton::clicked, this, &SettingsDialog::OnRebuildIndex);
  connect(btn_save_, &QPushButton::clicked, this, &SettingsDialog::OnSaveAndApply);
  connect(btn_cancel_, &QPushButton::clicked, this, [this]() {
    if (config_) {
      I18n::Instance().SetLanguage(config_->ui_language());
    }
    reject();
  });

  return bottom_layout;
}

void SettingsDialog::PopulateFromConfig() {
  if (!config_) return;
  for (auto *tab : tabs_) {
    tab->PopulateFromConfig(*config_);
  }
}

void SettingsDialog::RetranslateUi() {
  setWindowTitle(Tr("settings_title"));

  if (nav_list_) {
    const int count = std::min<int>(nav_list_->count(), static_cast<int>(nav_title_keys_.size()));
    for (int i = 0; i < count; ++i) {
      nav_list_->item(i)->setText(Tr(nav_title_keys_[i]));
    }
  }

  if (btn_rebuild_) btn_rebuild_->setText(Tr("btn_rebuild"));
  if (btn_save_) btn_save_->setText(Tr("btn_save"));
  if (btn_cancel_) btn_cancel_->setText(Tr("btn_cancel"));
  if (lock_banner_label_) lock_banner_label_->setText(Tr("settings_indexing_lock_banner"));
  if (btn_stop_indexing_) btn_stop_indexing_->setText(Tr("btn_stop_indexing"));

  for (auto *tab : tabs_) {
    tab->RetranslateUi();
  }
}

void SettingsDialog::OnLanguageChanged(const std::string &) {
  RetranslateUi();
}

void SettingsDialog::OnStopIndexingClicked() {
  if (!indexing_control_.cancel_indexing) {
    return;
  }
  if (btn_stop_indexing_) {
    btn_stop_indexing_->setEnabled(false);
  }
  indexing_control_.cancel_indexing();
}

void SettingsDialog::UpdateIndexingLockState() {
  const bool locked = indexing_control_.is_indexing && indexing_control_.is_indexing();
  if (lock_banner_) {
    lock_banner_->setVisible(locked);
  }
  if (btn_rebuild_) {
    btn_rebuild_->setEnabled(!locked);
  }
  if (btn_stop_indexing_ && (!locked || !was_structural_locked_)) {
    btn_stop_indexing_->setEnabled(true);
  }
  was_structural_locked_ = locked;
  for (auto *tab : tabs_) {
    tab->SetStructuralReadOnly(locked);
  }
}

void SettingsDialog::OnSaveAndApply() {
  if (!config_) {
    accept();
    return;
  }

  const bool locked = indexing_control_.is_indexing && indexing_control_.is_indexing();
  const service::IndexConfigSnapshot before_apply = config_->CaptureSnapshot();
  for (auto *tab : tabs_) {
    tab->SetStructuralReadOnly(locked);
    tab->ApplyToConfig(config_.get());
  }
  const service::ConfigChangeSummary diff = service::DiffIndexConfig(before_apply, config_->CaptureSnapshot());

  if (locked && !diff.HasAnyChanges()) {
    QMessageBox::information(this, Tr("dlg_tip"), Tr("settings_locked_no_runtime_changes"));
    return;
  }

  QString config_path;
  if (!config_->config_path().empty()) {
    config_path = QString::fromStdString(config_->config_path());
  } else {
    config_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/config.json";
  }

  if (!config_->SaveToFile(config_path.toStdString())) {
    QMessageBox::warning(this, Tr("dlg_error"), Tr("save_cfg_failed"));
    return;
  }

  emit SettingsAppliedWithDiff(diff);
  emit SettingsApplied();
  accept();
}

void SettingsDialog::OnRebuildIndex() {
  auto reply = QMessageBox::question(this, Tr("rebuild_confirm_title"), Tr("rebuild_confirm_msg"),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (reply == QMessageBox::Yes) {
    emit RebuildIndexRequested();
    accept();
  }
}

}  // namespace gui
}  // namespace soeasy
