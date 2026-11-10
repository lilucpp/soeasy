// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/search_bar.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QVBoxLayout>

#include "gui/i18n.h"
#include "gui/icon_provider.h"

namespace soeasy {
namespace gui {

SearchBar::SearchBar(QWidget *parent) : QWidget(parent) {
  SetupUi();
}

void SearchBar::SetupUi() {
  setObjectName("SearchControlPanel");

  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(12);

  // Row 1: Search Input (with Ctrl K) + Mode Segmented Control + Preview Toggle + Settings
  auto *row1_layout = new QHBoxLayout();
  row1_layout->setSpacing(10);

  // Input Wrap
  auto *input_wrap = new QFrame(this);
  input_wrap->setObjectName("SearchInputWrap");
  auto *input_layout = new QHBoxLayout(input_wrap);
  input_layout->setContentsMargins(8, 0, 8, 0);
  input_layout->setSpacing(8);

  query_edit_ = new QLineEdit(input_wrap);
  query_edit_->setObjectName("SearchInput");
  query_edit_->setPlaceholderText("键入关键词，毫秒级检索全文或文件名...");
  query_edit_->addAction(IconProvider::GetSearchIcon(), QLineEdit::LeadingPosition);
  query_edit_->setClearButtonEnabled(true);

  auto *kbd_hint = new QLabel("Ctrl K", input_wrap);
  kbd_hint->setProperty("role", "kbd-badge");

  input_layout->addWidget(query_edit_, 1);
  input_layout->addWidget(kbd_hint);

  // Segmented Mode Controls
  auto *seg_wrap = new QFrame(this);
  seg_wrap->setProperty("role", "segmented-group");
  auto *seg_layout = new QHBoxLayout(seg_wrap);
  seg_layout->setContentsMargins(2, 2, 2, 2);
  seg_layout->setSpacing(2);

  btn_mode_fulltext_ = new QPushButton("全文检索", seg_wrap);
  btn_mode_fulltext_->setProperty("role", "segment-btn");

  btn_mode_filename_ = new QPushButton("文件名", seg_wrap);
  btn_mode_filename_->setProperty("role", "segment-btn");

  seg_layout->addWidget(btn_mode_fulltext_);
  seg_layout->addWidget(btn_mode_filename_);

  // Preview Toggle Action
  btn_toggle_preview_ = new QPushButton("◨ 预览已开启", this);
  btn_toggle_preview_->setObjectName("BtnTogglePreview");
  btn_toggle_preview_->setProperty("role", "action-toggle");
  btn_toggle_preview_->setProperty("active", true);
  btn_toggle_preview_->setToolTip("切换右侧即时正文预览面板 (Ctrl+P)");

  // Settings Action
  btn_settings_ = new QPushButton("⚙ 设置中心", this);
  btn_settings_->setObjectName("BtnSettings");
  btn_settings_->setProperty("role", "action-toggle");
  btn_settings_->setToolTip("打开系统配置与偏好设置");

  row1_layout->addWidget(input_wrap, 1);
  row1_layout->addWidget(seg_wrap);
  row1_layout->addWidget(btn_toggle_preview_);
  row1_layout->addWidget(btn_settings_);
  main_layout->addLayout(row1_layout);

  // Row 2: Path Filter Scope Bar
  auto *row2_layout = new QHBoxLayout();
  row2_layout->setSpacing(10);

  label_scope_ = new QLabel("限定路径", this);
  label_scope_->setObjectName("LabelScope");

  auto *path_wrap = new QFrame(this);
  path_wrap->setObjectName("PathInputWrap");
  auto *path_layout = new QHBoxLayout(path_wrap);
  path_layout->setContentsMargins(8, 0, 8, 0);
  path_layout->setSpacing(8);

  scope_edit_ = new QLineEdit(path_wrap);
  scope_edit_->setObjectName("PathInput");
  scope_edit_->setPlaceholderText("留空搜索全部索引文件；输入目录以搜索其子目录");
  scope_edit_->setClearButtonEnabled(true);
  path_layout->addWidget(scope_edit_);

  btn_scope_browse_ = new QPushButton("浏览目录...", this);
  btn_scope_browse_->setObjectName("BtnScopeBrowse");
  btn_scope_browse_->setProperty("role", "secondary");

  btn_scope_clear_ = new QPushButton("清除范围", this);
  btn_scope_clear_->setObjectName("BtnScopeClear");
  btn_scope_clear_->setProperty("role", "secondary");

  row2_layout->addWidget(label_scope_);
  row2_layout->addWidget(path_wrap, 1);
  row2_layout->addWidget(btn_scope_browse_);
  row2_layout->addWidget(btn_scope_clear_);
  main_layout->addLayout(row2_layout);

  debounce_timer_ = new QTimer(this);
  debounce_timer_->setSingleShot(true);
  debounce_timer_->setInterval(180);
  connect(debounce_timer_, &QTimer::timeout, this, &SearchBar::OnQueryImmediate);

  connect(query_edit_, &QLineEdit::textChanged, this, &SearchBar::OnQueryTextChanged);
  connect(query_edit_, &QLineEdit::returnPressed, this, &SearchBar::OnQueryImmediate);
  connect(scope_edit_, &QLineEdit::textChanged, this, &SearchBar::OnQueryTextChanged);
  connect(scope_edit_, &QLineEdit::returnPressed, this, &SearchBar::OnQueryImmediate);

  connect(btn_mode_fulltext_, &QPushButton::clicked, this, &SearchBar::OnModeFulltextClicked);
  connect(btn_mode_filename_, &QPushButton::clicked, this, &SearchBar::OnModeFilenameClicked);

  connect(btn_scope_browse_, &QPushButton::clicked, this, &SearchBar::OnBrowseScope);
  connect(btn_scope_clear_, &QPushButton::clicked, this, &SearchBar::OnClearScope);
  connect(btn_settings_, &QPushButton::clicked, this, &SearchBar::SettingsRequested);
  connect(btn_toggle_preview_, &QPushButton::clicked, this, &SearchBar::PreviewToggleRequested);

  UpdateSegmentedButtons();
  RetranslateUi();
}

void SearchBar::OnModeFulltextClicked() {
  if (current_mode_ != kModeFulltext) {
    current_mode_ = kModeFulltext;
    UpdateSegmentedButtons();
    OnQueryImmediate();
  }
}

void SearchBar::OnModeFilenameClicked() {
  if (current_mode_ != kModeFilename) {
    current_mode_ = kModeFilename;
    UpdateSegmentedButtons();
    OnQueryImmediate();
  }
}

void SearchBar::UpdateSegmentedButtons() {
  if (btn_mode_fulltext_) {
    btn_mode_fulltext_->setProperty("active", current_mode_ == kModeFulltext);
    btn_mode_fulltext_->style()->unpolish(btn_mode_fulltext_);
    btn_mode_fulltext_->style()->polish(btn_mode_fulltext_);
  }
  if (btn_mode_filename_) {
    btn_mode_filename_->setProperty("active", current_mode_ == kModeFilename);
    btn_mode_filename_->style()->unpolish(btn_mode_filename_);
    btn_mode_filename_->style()->polish(btn_mode_filename_);
  }
}

void SearchBar::SetPreviewActive(bool active) {
  if (btn_toggle_preview_) {
    btn_toggle_preview_->setProperty("active", active);
    btn_toggle_preview_->setText(active ? Tr("btn_preview") : Tr("btn_preview_off"));
    btn_toggle_preview_->style()->unpolish(btn_toggle_preview_);
    btn_toggle_preview_->style()->polish(btn_toggle_preview_);
  }
}

void SearchBar::RetranslateUi() {
  if (query_edit_) {
    query_edit_->setPlaceholderText(Tr("search_placeholder"));
  }
  if (btn_mode_fulltext_) {
    btn_mode_fulltext_->setText(Tr("mode_fulltext"));
  }
  if (btn_mode_filename_) {
    btn_mode_filename_->setText(Tr("mode_filename"));
  }
  if (btn_toggle_preview_) {
    btn_toggle_preview_->setText(Tr("btn_preview"));
    btn_toggle_preview_->setToolTip(Tr("tip_preview"));
  }
  if (btn_settings_) {
    btn_settings_->setText(Tr("btn_settings"));
  }
  if (label_scope_) {
    label_scope_->setText(Tr("label_scope"));
  }
  if (scope_edit_) {
    scope_edit_->setPlaceholderText(Tr("scope_placeholder"));
  }
  if (btn_scope_browse_) {
    btn_scope_browse_->setText(Tr("browse"));
  }
  if (btn_scope_clear_) {
    btn_scope_clear_->setText(Tr("btn_clear_scope"));
  }
}

QString SearchBar::query() const {
  return query_edit_ ? query_edit_->text().trimmed() : QString();
}

SearchMode SearchBar::mode() const {
  return current_mode_;
}

QString SearchBar::path_scope() const {
  return scope_edit_ ? scope_edit_->text().trimmed() : QString();
}

void SearchBar::SetQuery(const QString &query) {
  if (query_edit_) {
    query_edit_->setText(query);
    OnQueryImmediate();
  }
}

void SearchBar::SetMode(SearchMode mode) {
  current_mode_ = mode;
  UpdateSegmentedButtons();
  OnQueryImmediate();
}

void SearchBar::SetPathScope(const QString &path) {
  if (scope_edit_) {
    scope_edit_->setText(path);
    OnQueryImmediate();
  }
}

void SearchBar::OnBrowseScope() {
  QString initial_dir = scope_edit_->text().trimmed();
  if (initial_dir.isEmpty() || !QDir(initial_dir).exists()) {
    initial_dir = QDir::homePath();
  }
  QString dir = QFileDialog::getExistingDirectory(this, "选择搜索范围目录", initial_dir,
                                                  QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (!dir.isEmpty()) {
    scope_edit_->setText(dir);
    OnQueryImmediate();
  }
}

void SearchBar::OnClearScope() {
  scope_edit_->clear();
  OnQueryImmediate();
}

void SearchBar::OnQueryTextChanged() {
  debounce_timer_->start();
}

void SearchBar::OnQueryImmediate() {
  debounce_timer_->stop();
  emit SearchTriggered(query(), static_cast<int>(current_mode_), path_scope());
}

}  // namespace gui
}  // namespace soeasy
