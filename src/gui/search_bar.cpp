#include "gui/search_bar.h"

#include <QAction>
#include <QHBoxLayout>
#include <QKeyEvent>

#include "gui/i18n.h"
#include "gui/icon_provider.h"

namespace soeasy {
namespace gui {

SearchBar::SearchBar(QWidget *parent) : QWidget(parent) {
  SetupUi();
}

void SearchBar::SetupUi() {
  auto *main_layout = new QHBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(8);

  // 1. Search Mode Switcher (全文倒排 / 文件名秒搜)
  mode_combo_ = new QComboBox(this);
  mode_combo_->addItem("全文倒排 (FTS5)", kModeFulltext);
  mode_combo_->addItem("文件名秒搜", kModeFilename);
  mode_combo_->setStyleSheet(
      "QComboBox {"
      "  font-size: 13px;"
      "  padding: 5px 28px 5px 12px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  color: #1E293B;"
      "  min-height: 22px;"
      "}"
      "QComboBox:hover {"
      "  border-color: #94A3B8;"
      "}"
      "QComboBox:focus, QComboBox:on {"
      "  border: 1.5px solid #0284C7;"
      "}"
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
      "  outline: none;"
      "  padding: 4px;"
      "}");

  // 2. Query Input Box
  query_edit_ = new QLineEdit(this);
  query_edit_->setPlaceholderText("输入搜索关键词（支持 FTS5 语法：AND, OR, NOT, \"精确词组\", 前缀*）...");
  query_edit_->addAction(IconProvider::GetSearchIcon(), QLineEdit::LeadingPosition);
  query_edit_->setClearButtonEnabled(true);
  query_edit_->installEventFilter(this);
  query_edit_->setStyleSheet(
      "QLineEdit {"
      "  font-size: 13px;"
      "  padding: 6px 10px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  color: #1E293B;"
      "  min-height: 20px;"
      "}"
      "QLineEdit:focus {"
      "  border: 1.5px solid #0284C7;"
      "}");

  // 3. System Settings Button
  btn_settings_ = new QPushButton("⚙ 系统设置", this);
  btn_settings_->setCursor(Qt::PointingHandCursor);
  btn_settings_->setStyleSheet(
      "QPushButton {"
      "  font-size: 13px;"
      "  padding: 5px 14px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #F8FAFC;"
      "  color: #334155;"
      "  min-height: 22px;"
      "}"
      "QPushButton:hover {"
      "  background-color: #F1F5F9;"
      "  border-color: #94A3B8;"
      "  color: #0F172A;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #E2E8F0;"
      "}");

  // Single streamlined horizontal bar
  main_layout->addWidget(mode_combo_);
  main_layout->addWidget(query_edit_, 1);
  main_layout->addWidget(btn_settings_);

  // Debounce Timer
  debounce_timer_ = new QTimer(this);
  debounce_timer_->setSingleShot(true);
  debounce_timer_->setInterval(180);  // 180ms smooth debounce window
  connect(debounce_timer_, &QTimer::timeout, this, &SearchBar::OnQueryImmediate);

  // Signal Connections
  connect(query_edit_, &QLineEdit::textChanged, this, &SearchBar::OnQueryTextChanged);
  connect(query_edit_, &QLineEdit::returnPressed, this, &SearchBar::OnQueryImmediate);
  connect(mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SearchBar::OnQueryImmediate);
  connect(btn_settings_, &QPushButton::clicked, this, &SearchBar::SettingsRequested);

  RetranslateUi();
}

void SearchBar::RetranslateUi() {
  if (query_edit_) {
    query_edit_->setPlaceholderText(Tr("search_placeholder"));
  }
  if (mode_combo_) {
    int current_idx = mode_combo_->currentIndex();
    mode_combo_->blockSignals(true);
    mode_combo_->clear();
    mode_combo_->addItem(Tr("mode_fulltext"), kModeFulltext);
    mode_combo_->addItem(Tr("mode_filename"), kModeFilename);
    if (current_idx >= 0 && current_idx < mode_combo_->count()) {
      mode_combo_->setCurrentIndex(current_idx);
    }
    mode_combo_->blockSignals(false);
  }
  if (btn_settings_) {
    btn_settings_->setText(Tr("btn_settings"));
  }
}

QString SearchBar::query() const {
  return query_edit_->text().trimmed();
}

SearchMode SearchBar::mode() const {
  return static_cast<SearchMode>(mode_combo_->currentData().toInt());
}

void SearchBar::SetQuery(const QString &query) {
  query_edit_->setText(query);
  OnQueryImmediate();
}

void SearchBar::SetMode(SearchMode mode) {
  int idx = mode_combo_->findData(static_cast<int>(mode));
  if (idx >= 0) {
    mode_combo_->setCurrentIndex(idx);
  }
}

void SearchBar::FocusQuery() {
  if (query_edit_) {
    query_edit_->setFocus();
    query_edit_->selectAll();
  }
}

void SearchBar::ClearQuery() {
  if (query_edit_) {
    query_edit_->clear();
    query_edit_->setFocus();
  }
}

bool SearchBar::eventFilter(QObject *watched, QEvent *event) {
  if (watched == query_edit_ && event->type() == QEvent::KeyPress) {
    auto *key_event = static_cast<QKeyEvent *>(event);
    if (key_event->key() == Qt::Key_Escape) {
      if (!query_edit_->text().isEmpty()) {
        ClearQuery();
        return true;
      }
    } else if (key_event->key() == Qt::Key_Down) {
      emit NavigateResultsRequested();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
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
