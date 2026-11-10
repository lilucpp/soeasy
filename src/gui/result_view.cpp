// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/result_view.h"

#include <QHeaderView>
#include <QItemSelectionModel>
#include <QScrollBar>

#include "gui/result_item_delegate.h"

namespace soeasy {
namespace gui {

ResultView::ResultView(QWidget *parent) : QTableView(parent) {
  model_ = new SearchTableModel(this);
  setModel(model_);

  SetupTable();

  hover_popup_ = new HoverPreviewPopup(this);
  hover_timer_ = new QTimer(this);
  hover_timer_->setSingleShot(true);
  connect(hover_timer_, &QTimer::timeout, this, &ResultView::OnHoverTimerFired);
}

void ResultView::SetupTable() {
  delegate_ = new ResultItemDelegate(this);
  setItemDelegate(delegate_);

  verticalHeader()->setVisible(false);
  verticalHeader()->setDefaultSectionSize(54);
  setIconSize(QSize(24, 24));
  setShowGrid(false);

  // Align with Figma design:
  // Hide pure index, path and score columns from visual header (path is rendered within card delegate)
  setColumnHidden(SearchTableModel::kColIndex, true);
  setColumnHidden(SearchTableModel::kColPath, true);
  setColumnHidden(SearchTableModel::kColScore, true);

  horizontalHeader()->setSectionResizeMode(SearchTableModel::kColFileName, QHeaderView::Stretch);
  horizontalHeader()->setSectionResizeMode(SearchTableModel::kColSize, QHeaderView::Fixed);
  horizontalHeader()->setSectionResizeMode(SearchTableModel::kColMtime, QHeaderView::Fixed);

  setColumnWidth(SearchTableModel::kColSize, 85);
  setColumnWidth(SearchTableModel::kColMtime, 135);

  setSelectionBehavior(QAbstractItemView::SelectRows);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setEditTriggers(QAbstractItemView::NoEditTriggers);
  setAlternatingRowColors(false);
  setMouseTracking(true);
  setAutoFillBackground(true);
  QPalette pal = palette();
  pal.setColor(QPalette::Base, QColor("#FFFFFF"));
  pal.setColor(QPalette::Window, QColor("#FFFFFF"));
  setPalette(pal);

  setObjectName("ResultView");

  connect(this, &QTableView::doubleClicked, this, &ResultView::OnDoubleClicked);

  if (selectionModel()) {
    connect(selectionModel(), &QItemSelectionModel::currentChanged, this, &ResultView::OnCurrentChanged);
  }

  // Viewport scroll lazy loading
  scroll_timer_ = new QTimer(this);
  scroll_timer_->setSingleShot(true);
  scroll_timer_->setInterval(60);
  connect(scroll_timer_, &QTimer::timeout, this, &ResultView::OnScrollIdle);
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
    if (scroll_timer_) {
      scroll_timer_->start(60);
    }
    int max_val = verticalScrollBar()->maximum();
    if (max_val > 0 && value >= max_val - 10) {
      emit LoadMoreRequested();
    }
  });
}

void ResultView::OnDoubleClicked(const QModelIndex &index) {
  hover_popup_->HidePopup();
  hover_timer_->stop();

  if (index.isValid() && model_) {
    const auto *it = model_->GetItem(index.row());
    if (it) {
      emit FileOpenRequested(it->doc_id, it->file_name, it->file_path);
    }
  }
}

void ResultView::OnCurrentChanged(const QModelIndex &current, const QModelIndex & /*previous*/) {
  if (current.isValid() && model_) {
    const auto *it = model_->GetItem(current.row());
    if (it) {
      emit FileSelected(it->doc_id, it->file_name, it->file_path);
    }
  }
}

void ResultView::SetStorageEngine(std::shared_ptr<storage::StorageEngine> storage) {
  storage_ = std::move(storage);
}

void ResultView::SetCurrentQuery(const QString &query) {
  if (current_query_ != query) {
    current_query_ = query;
    snippet_lru_cache_.clear();
    if (delegate_) {
      delegate_->SetCurrentQuery(query);
    }
    viewport()->update();
  }
}

void ResultView::ClearResults() {
  hover_popup_->HidePopup();
  hover_timer_->stop();
  if (scroll_timer_) {
    scroll_timer_->stop();
  }
  hover_row_ = -1;
  if (model_) {
    model_->ClearResults();
  }
}

void ResultView::SortResults(int type) {
  if (model_) {
    model_->SortResults(type);
    if (model_->rowCount() > 0) {
      selectRow(0);
    }
  }
}

void ResultView::OnScrollIdle() {
  if (!model_ || model_->items().empty() || !storage_ || current_query_.isEmpty()) {
    return;
  }
  int top_row = rowAt(4);
  int bottom_row = rowAt(viewport()->height() - 4);
  if (top_row < 0) top_row = 0;
  if (bottom_row < 0) bottom_row = model_->rowCount() - 1;

  int start_row = std::max(0, top_row - 2);
  int end_row = std::min(model_->rowCount() - 1, bottom_row + 2);
  EnsureSnippetForRows(start_row, end_row);
}

void ResultView::EnsureSnippetForRows(int start_row, int end_row) {
  if (!model_ || !storage_ || current_query_.isEmpty() || model_->items().empty()) {
    return;
  }
  start_row = std::max(0, start_row);
  end_row = std::min(model_->rowCount() - 1, end_row);

  auto keywords = storage::StorageEngine::ExtractKeywordsFromQuery(current_query_.toStdString());
  if (keywords.empty()) return;

  for (int r = start_row; r <= end_row; ++r) {
    auto *item = model_->GetItemMutable(r);
    if (!item || item->has_body_snippet) continue;

    auto it = snippet_lru_cache_.find(item->doc_id);
    if (it != snippet_lru_cache_.end()) {
      item->snippet_html = QString::fromStdString(it->second);
      item->has_body_snippet = true;
      continue;
    }

    std::string full_text;
    if (storage_->GetDocumentContent(item->doc_id, &full_text) && !full_text.empty()) {
      std::string snip = storage::StorageEngine::GenerateSnippet(full_text, keywords, 140);
      if (!snip.empty()) {
        if (snippet_lru_cache_.size() >= 300) {
          snippet_lru_cache_.erase(snippet_lru_cache_.begin());
        }
        snippet_lru_cache_[item->doc_id] = snip;
        item->snippet_html = QString::fromStdString(snip);
        item->has_body_snippet = true;
      }
    }
  }
}

void ResultView::SetResults(const std::vector<DisplayResultItem> &items) {
  ClearResults();
  if (model_) {
    model_->SetResults(items);
  }
}

void ResultView::AppendResults(const std::vector<DisplayResultItem> &items) {
  if (items.empty() || !model_) {
    return;
  }
  model_->AppendResults(items);
}

void ResultView::mouseMoveEvent(QMouseEvent *event) {
  QTableView::mouseMoveEvent(event);

  int row = rowAt(event->pos().y());
  if (row != hover_row_) {
    hover_popup_->HidePopup();
    hover_row_ = row;
    hover_pos_ = event->globalPos();

    if (model_ && hover_row_ >= 0 && hover_row_ < model_->rowCount()) {
      hover_timer_->start(150);  // 150ms hover delay
    } else {
      hover_timer_->stop();
    }
  }
}

void ResultView::leaveEvent(QEvent *event) {
  QTableView::leaveEvent(event);
  hover_popup_->HidePopup();
  hover_timer_->stop();
  hover_row_ = -1;
}

void ResultView::OnHoverTimerFired() {
  if (model_ && hover_row_ >= 0 && hover_row_ < model_->rowCount()) {
    EnsureSnippetForRows(hover_row_, hover_row_);
    const auto *it = model_->GetItem(hover_row_);
    if (it) {
      hover_popup_->ShowSnippet(hover_pos_, it->file_name, it->file_path, it->snippet_html, it->file_size, it->mtime,
                                it->score);
    }
  }
}

std::unique_ptr<ResultView::CompatItemProxy> ResultView::item(int row, int col) const {
  if (!model_ || row < 0 || row >= model_->rowCount()) {
    return nullptr;
  }
  QString text = model_->data(model_->index(row, col), Qt::DisplayRole).toString();
  return std::make_unique<CompatItemProxy>(CompatItemProxy{text});
}

std::unique_ptr<ResultView::CompatItemProxy> ResultView::horizontalHeaderItem(int section) const {
  if (!model_ || section < 0 || section >= model_->columnCount()) {
    return nullptr;
  }
  QString text = model_->headerData(section, Qt::Horizontal, Qt::DisplayRole).toString();
  return std::make_unique<CompatItemProxy>(CompatItemProxy{text});
}

void ResultView::RetranslateUi() {
  if (model_) {
    model_->RetranslateUi();
  }
}

}  // namespace gui
}  // namespace soeasy
