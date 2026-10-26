#include "gui/search_bar.h"
#include "gui/icon_provider.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace soeasy {
namespace gui {

SearchBar::SearchBar(QWidget* parent) : QWidget(parent) {
  SetupUi();
}

void SearchBar::SetupUi() {
  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(8);

  // Row 1: Query Input + Mode Selector + Settings Button
  auto* query_layout = new QHBoxLayout();
  query_layout->setSpacing(8);

  query_edit_ = new QLineEdit(this);
  query_edit_->setPlaceholderText(
      "输入搜索关键词（支持 FTS5 语法：AND, OR, NOT, \"精确词组\", 前缀*）...");
  query_edit_->addAction(IconProvider::GetSearchIcon(), QLineEdit::LeadingPosition);
  query_edit_->setClearButtonEnabled(true);
  query_edit_->setStyleSheet(
      "QLineEdit {"
      "  font-size: 13px;"
      "  padding: 6px 10px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  color: #1E293B;"
      "}"
      "QLineEdit:focus {"
      "  border: 1.5px solid #0284C7;"
      "}");

  mode_combo_ = new QComboBox(this);
  mode_combo_->addItem("全文倒排 (FTS5)", kModeFulltext);
  mode_combo_->addItem("文件名秒搜", kModeFilename);
  QString arrow_path = IconProvider::GetDownArrowImagePath();
  mode_combo_->setStyleSheet(QString(
      "QComboBox {"
      "  font-size: 13px;"
      "  padding: 5px 28px 5px 12px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  color: #1E293B;"
      "}"
      "QComboBox:hover {"
      "  border-color: #94A3B8;"
      "}"
      "QComboBox:focus {"
      "  border: 1.5px solid #0284C7;"
      "}"
      "QComboBox::drop-down {"
      "  subcontrol-origin: padding;"
      "  subcontrol-position: top right;"
      "  width: 24px;"
      "  border-left: none;"
      "}"
      "QComboBox::down-arrow {"
      "  image: url(%1);"
      "  width: 12px;"
      "  height: 8px;"
      "  margin-right: 8px;"
      "}"
      "QComboBox QAbstractItemView {"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  selection-background-color: #BDDDF8;"
      "  selection-color: #0F172A;"
      "  outline: none;"
      "  padding: 4px;"
      "}").arg(arrow_path));

  btn_toggle_preview_ = new QPushButton("◨ 预览", this);
  btn_toggle_preview_->setToolTip("切换右侧即时正文预览面板 (Ctrl+P)");
  btn_toggle_preview_->setStyleSheet(
      "QPushButton {"
      "  font-size: 13px;"
      "  padding: 5px 12px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #DCE8F5;"
      "  color: #1E293B;"
      "}"
      "QPushButton:hover {"
      "  background-color: #CBDCEF;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #B8CFE5;"
      "}");

  btn_settings_ = new QPushButton("⚙ 设置中心", this);
  btn_settings_->setStyleSheet(
      "QPushButton {"
      "  font-size: 13px;"
      "  padding: 5px 14px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #DCE8F5;"
      "  color: #1E293B;"
      "}"
      "QPushButton:hover {"
      "  background-color: #CBDCEF;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #B8CFE5;"
      "}");

  query_layout->addWidget(query_edit_, 1);
  query_layout->addWidget(mode_combo_);
  query_layout->addWidget(btn_toggle_preview_);
  query_layout->addWidget(btn_settings_);
  main_layout->addLayout(query_layout);

  // Row 2: Search Path Scope constraint
  auto* scope_layout = new QHBoxLayout();
  scope_layout->setSpacing(6);

  auto* label_scope = new QLabel("限定搜索路径:", this);
  label_scope->setStyleSheet("font-size: 13px; color: #1E293B; font-weight: bold;");

  scope_edit_ = new QLineEdit(this);
  scope_edit_->setPlaceholderText("留空表示全盘/全局检索；填入路径则仅在该目录及其子树下检索...");
  scope_edit_->setStyleSheet(
      "QLineEdit {"
      "  font-size: 12px;"
      "  padding: 5px 8px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  color: #1E293B;"
      "}"
      "QLineEdit:focus {"
      "  border: 1.5px solid #0284C7;"
      "}");

  btn_scope_browse_ = new QPushButton("浏览...", this);
  btn_scope_browse_->setStyleSheet(
      "QPushButton {"
      "  font-size: 12px;"
      "  padding: 5px 14px;"
      "  border: 1px solid #7DD3FC;"
      "  border-radius: 6px;"
      "  background-color: #BAE6FD;"
      "  color: #0369A1;"
      "  font-weight: 500;"
      "}"
      "QPushButton:hover {"
      "  background-color: #93C5FD;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #7DD3FC;"
      "}");

  btn_scope_clear_ = new QPushButton("清除范围", this);
  btn_scope_clear_->setStyleSheet(
      "QPushButton {"
      "  font-size: 12px;"
      "  padding: 5px 12px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #DCE8F5;"
      "  color: #1E293B;"
      "}"
      "QPushButton:hover {"
      "  background-color: #CBDCEF;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #B8CFE5;"
      "}");

  scope_layout->addWidget(label_scope);
  scope_layout->addWidget(scope_edit_, 1);
  scope_layout->addWidget(btn_scope_browse_);
  scope_layout->addWidget(btn_scope_clear_);
  main_layout->addLayout(scope_layout);

  debounce_timer_ = new QTimer(this);
  debounce_timer_->setSingleShot(true);
  debounce_timer_->setInterval(180);  // 180ms smooth debounce window
  connect(debounce_timer_, &QTimer::timeout, this, &SearchBar::OnQueryImmediate);

  connect(query_edit_, &QLineEdit::textChanged, this, &SearchBar::OnQueryTextChanged);
  connect(query_edit_, &QLineEdit::returnPressed, this, &SearchBar::OnQueryImmediate);
  connect(mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SearchBar::OnQueryImmediate);
  connect(scope_edit_, &QLineEdit::textChanged, this, &SearchBar::OnQueryTextChanged);
  connect(scope_edit_, &QLineEdit::returnPressed, this, &SearchBar::OnQueryImmediate);

  connect(btn_scope_browse_, &QPushButton::clicked, this, &SearchBar::OnBrowseScope);
  connect(btn_scope_clear_, &QPushButton::clicked, this, &SearchBar::OnClearScope);
  connect(btn_settings_, &QPushButton::clicked, this, &SearchBar::SettingsRequested);
  connect(btn_toggle_preview_, &QPushButton::clicked, this, &SearchBar::PreviewToggleRequested);
}

QString SearchBar::query() const {
  return query_edit_->text().trimmed();
}

SearchMode SearchBar::mode() const {
  return static_cast<SearchMode>(mode_combo_->currentData().toInt());
}

QString SearchBar::path_scope() const {
  return scope_edit_->text().trimmed();
}

void SearchBar::SetQuery(const QString& query) {
  query_edit_->setText(query);
  OnQueryImmediate();
}

void SearchBar::SetMode(SearchMode mode) {
  int idx = mode_combo_->findData(static_cast<int>(mode));
  if (idx >= 0) {
    mode_combo_->setCurrentIndex(idx);
  }
}

void SearchBar::SetPathScope(const QString& path) {
  scope_edit_->setText(path);
}

void SearchBar::OnBrowseScope() {
  QString dir = QFileDialog::getExistingDirectory(
      this, "选择限定检索路径", scope_edit_->text().isEmpty() ? QDir::homePath() : scope_edit_->text(),
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (!dir.isEmpty()) {
    scope_edit_->setText(dir);
  }
}

void SearchBar::OnClearScope() {
  scope_edit_->clear();
}

void SearchBar::OnQueryTextChanged() {
  if (debounce_timer_) {
    debounce_timer_->start(180);
  }
}

void SearchBar::OnQueryImmediate() {
  if (debounce_timer_) {
    debounce_timer_->stop();
  }
  emit SearchTriggered(query(), mode(), path_scope());
}

}  // namespace gui
}  // namespace soeasy
