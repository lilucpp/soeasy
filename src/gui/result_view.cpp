#include "gui/result_view.h"
#include "gui/icon_provider.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHeaderView>
#include <QScrollBar>
#include <QTableWidgetItem>

namespace soeasy {
namespace gui {

namespace {

QString FormatFileSize(uint64_t bytes) {
  if (bytes < 1024) {
    return QString("%1 B").arg(bytes);
  }
  if (bytes < 1024 * 1024) {
    return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
  }
  return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

}  // namespace

ResultView::ResultView(QWidget* parent) : QTableWidget(parent) {
  SetupTable();

  hover_popup_ = new HoverPreviewPopup(this);
  hover_timer_ = new QTimer(this);
  hover_timer_->setSingleShot(true);
  connect(hover_timer_, &QTimer::timeout, this, &ResultView::OnHoverTimerFired);
}

void ResultView::SetupTable() {
  setColumnCount(5);
  QStringList headers;
  headers << "文件名称" << "所在路径" << "大小" << "修改时间" << "相关度评分";
  setHorizontalHeaderLabels(headers);

  if (auto* item = horizontalHeaderItem(2)) {
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
  }
  if (auto* item = horizontalHeaderItem(3)) {
    item->setTextAlignment(Qt::AlignCenter);
  }
  if (auto* item = horizontalHeaderItem(4)) {
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
  }

  horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
  horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

  setColumnWidth(0, 260);
  verticalHeader()->setVisible(false);
  verticalHeader()->setDefaultSectionSize(34);
  setIconSize(QSize(18, 18));
  setShowGrid(false);

  setSelectionBehavior(QAbstractItemView::SelectRows);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setEditTriggers(QAbstractItemView::NoEditTriggers);
  setAlternatingRowColors(true);
  setMouseTracking(true);

  setStyleSheet(
      "QTableWidget {"
      "  font-size: 13px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  alternate-background-color: #F4F8FC;"
      "  selection-background-color: #BDDDF8;"
      "  selection-color: #0F172A;"
      "  outline: none;"
      "}"
      "QTableWidget::item {"
      "  padding-left: 8px;"
      "  padding-right: 8px;"
      "  border: none;"
      "}"
      "QTableWidget::item:hover {"
      "  background-color: #EBF3FA;"
      "}"
      "QHeaderView::section {"
      "  background-color: #DCE8F5;"
      "  font-size: 12px;"
      "  font-weight: bold;"
      "  color: #1E293B;"
      "  padding: 7px 8px;"
      "  border: none;"
      "  border-right: 1px solid #CBD5E1;"
      "  border-bottom: 1px solid #CBD5E1;"
      "}"
      "QScrollBar:vertical {"
      "  width: 6px;"
      "  background: transparent;"
      "  margin: 0px;"
      "  padding: 0px;"
      "}"
      "QScrollBar::handle:vertical {"
      "  background: #CBD5E1;"
      "  min-height: 24px;"
      "  border-radius: 3px;"
      "}"
      "QScrollBar::handle:vertical:hover {"
      "  background: #94A3B8;"
      "}"
      "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
      "  height: 0px;"
      "  background: transparent;"
      "}"
      "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
      "  background: transparent;"
      "}"
      "QScrollBar:horizontal {"
      "  height: 6px;"
      "  background: transparent;"
      "  margin: 0px;"
      "  padding: 0px;"
      "}"
      "QScrollBar::handle:horizontal {"
      "  background: #CBD5E1;"
      "  min-width: 24px;"
      "  border-radius: 3px;"
      "}"
      "QScrollBar::handle:horizontal:hover {"
      "  background: #94A3B8;"
      "}"
      "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
      "  width: 0px;"
      "  background: transparent;"
      "}"
      "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
      "  background: transparent;"
      "}");

  connect(this, &QTableWidget::cellDoubleClicked, this,
          &ResultView::OnItemDoubleClicked);
  connect(this, &QTableWidget::currentCellChanged, this,
          [this](int currentRow, int, int, int) {
            if (currentRow >= 0 && currentRow < static_cast<int>(current_items_.size())) {
              const auto& it = current_items_[currentRow];
              emit FileSelected(it.doc_id, it.file_name, it.file_path);
            }
          });

  // Viewport scroll lazy loading
  scroll_timer_ = new QTimer(this);
  scroll_timer_->setSingleShot(true);
  scroll_timer_->setInterval(60);
  connect(scroll_timer_, &QTimer::timeout, this, &ResultView::OnScrollIdle);
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
    if (scroll_timer_) {
      scroll_timer_->start(60);
    }
  });
}

void ResultView::SetStorageEngine(
    std::shared_ptr<storage::StorageEngine> storage) {
  storage_ = std::move(storage);
}

void ResultView::SetCurrentQuery(const QString& query) {
  if (current_query_ != query) {
    current_query_ = query;
    snippet_lru_cache_.clear();
  }
}

void ResultView::ClearResults() {
  hover_popup_->HidePopup();
  hover_timer_->stop();
  if (scroll_timer_) {
    scroll_timer_->stop();
  }
  hover_row_ = -1;
  current_items_.clear();
  setRowCount(0);
}

void ResultView::OnScrollIdle() {
  if (current_items_.empty() || !storage_ || current_query_.isEmpty()) {
    return;
  }
  int top_row = rowAt(4);
  int bottom_row = rowAt(viewport()->height() - 4);
  if (top_row < 0) top_row = 0;
  if (bottom_row < 0) bottom_row = static_cast<int>(current_items_.size()) - 1;

  int start_row = std::max(0, top_row - 2);
  int end_row = std::min(static_cast<int>(current_items_.size()) - 1, bottom_row + 2);
  EnsureSnippetForRows(start_row, end_row);
}

void ResultView::EnsureSnippetForRows(int start_row, int end_row) {
  if (!storage_ || current_query_.isEmpty() || current_items_.empty()) {
    return;
  }
  start_row = std::max(0, start_row);
  end_row = std::min(static_cast<int>(current_items_.size()) - 1, end_row);

  auto keywords =
      storage::StorageEngine::ExtractKeywordsFromQuery(current_query_.toStdString());
  if (keywords.empty()) return;

  for (int r = start_row; r <= end_row; ++r) {
    auto& item = current_items_[r];
    if (item.has_body_snippet) continue;

    auto it = snippet_lru_cache_.find(item.doc_id);
    if (it != snippet_lru_cache_.end()) {
      item.snippet_html = QString::fromStdString(it->second);
      item.has_body_snippet = true;
      continue;
    }

    std::string full_text;
    if (storage_->GetDocumentContent(item.doc_id, &full_text) && !full_text.empty()) {
      std::string snip =
          storage::StorageEngine::GenerateSnippet(full_text, keywords, 140);
      if (!snip.empty()) {
        if (snippet_lru_cache_.size() >= 300) {
          snippet_lru_cache_.erase(snippet_lru_cache_.begin());
        }
        snippet_lru_cache_[item.doc_id] = snip;
        item.snippet_html = QString::fromStdString(snip);
        item.has_body_snippet = true;
      }
    }
  }
}

void ResultView::SetResults(const std::vector<DisplayResultItem>& items) {
  ClearResults();
  current_items_ = items;
  setRowCount(static_cast<int>(items.size()));

  for (size_t i = 0; i < items.size(); ++i) {
    const auto& it = items[i];
    int row = static_cast<int>(i);

    QFileInfo fi(it.file_path);
    QString ext = it.ext.isEmpty() ? fi.suffix() : it.ext;

    auto* name_item = new QTableWidgetItem(it.file_name);
    name_item->setIcon(IconProvider::GetFileIcon(ext));
    name_item->setFont(QFont(font().family(), font().pointSize(), QFont::Normal));

    auto* path_item = new QTableWidgetItem(fi.absolutePath());
    path_item->setForeground(QColor("#64748B"));

    auto* size_item = new QTableWidgetItem(FormatFileSize(it.file_size));
    size_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QString date_str =
        QDateTime::fromSecsSinceEpoch(it.mtime).toString("yyyy-MM-dd hh:mm");
    auto* date_item = new QTableWidgetItem(date_str);
    date_item->setTextAlignment(Qt::AlignCenter);

    auto* score_item = new QTableWidgetItem(QString::number(it.score, 'f', 2));
    score_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

    setItem(row, 0, name_item);
    setItem(row, 1, path_item);
    setItem(row, 2, size_item);
    setItem(row, 3, date_item);
    setItem(row, 4, score_item);
  }
}

void ResultView::mouseMoveEvent(QMouseEvent* event) {
  QTableWidget::mouseMoveEvent(event);

  int row = rowAt(event->pos().y());
  if (row != hover_row_) {
    hover_popup_->HidePopup();
    hover_row_ = row;
    hover_pos_ = event->globalPos();

    if (hover_row_ >= 0 && hover_row_ < static_cast<int>(current_items_.size())) {
      hover_timer_->start(150);  // 150ms hover delay
    } else {
      hover_timer_->stop();
    }
  }
}

void ResultView::leaveEvent(QEvent* event) {
  QTableWidget::leaveEvent(event);
  hover_popup_->HidePopup();
  hover_timer_->stop();
  hover_row_ = -1;
}

void ResultView::OnHoverTimerFired() {
  if (hover_row_ >= 0 && hover_row_ < static_cast<int>(current_items_.size())) {
    EnsureSnippetForRows(hover_row_, hover_row_);
    const auto& it = current_items_[hover_row_];
    hover_popup_->ShowSnippet(hover_pos_, it.file_name, it.file_path,
                              it.snippet_html, it.file_size, it.mtime,
                              it.score);
  }
}

void ResultView::OnItemDoubleClicked(int row, int /*column*/) {
  hover_popup_->HidePopup();
  hover_timer_->stop();

  if (row >= 0 && row < static_cast<int>(current_items_.size())) {
    const auto& it = current_items_[row];
    emit FileOpenRequested(it.doc_id, it.file_name, it.file_path);
  }
}

}  // namespace gui
}  // namespace soeasy
