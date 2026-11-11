// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/result_view.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>

namespace soeasy {
namespace gui {

namespace {

class FileNameAndPathDelegate : public QStyledItemDelegate {
 public:
  explicit FileNameAndPathDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    // Draw standard selection / hover background
    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

    const bool is_selected = (opt.state & QStyle::State_Selected);

    // Fetch icon, name, path
    QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    QString file_name = index.data(Qt::DisplayRole).toString();
    QString file_path = index.data(Qt::UserRole + 1).toString();
    if (file_path.isEmpty()) {
      file_path = index.data(Qt::ToolTipRole).toString();
    }

    const int icon_size = 22;
    const int padding_h = 10;
    const int icon_x = opt.rect.left() + padding_h;
    const int icon_y = opt.rect.top() + (opt.rect.height() - icon_size) / 2;

    if (!icon.isNull()) {
      icon.paint(painter, icon_x, icon_y, icon_size, icon_size);
    }

    const int text_x = icon_x + icon_size + 10;
    const int text_w = opt.rect.right() - text_x - padding_h;

    if (text_w > 0) {
      // 1. File Name (Top Line)
      QFont name_font = opt.font;
      name_font.setPointSizeF(name_font.pointSizeF() > 0 ? name_font.pointSizeF() : 10.0);
      name_font.setWeight(QFont::DemiBold);
      painter->setFont(name_font);
      painter->setPen(is_selected ? QColor("#0284C7") : QColor("#0F172A"));

      QFontMetrics fm_name(name_font);
      QString elided_name = fm_name.elidedText(file_name, Qt::ElideRight, text_w);
      int top_y = opt.rect.top() + 5;
      painter->drawText(QRect(text_x, top_y, text_w, fm_name.height()),
                       Qt::AlignLeft | Qt::AlignVCenter, elided_name);

      // 2. File Path (Bottom Line)
      QFont path_font = opt.font;
      path_font.setPointSizeF(std::max(7.5, (name_font.pointSizeF() > 0 ? name_font.pointSizeF() : 10.0) - 1.5));
      path_font.setWeight(QFont::Normal);
      painter->setFont(path_font);
      painter->setPen(is_selected ? QColor("#0369A1") : QColor("#64748B"));

      QFontMetrics fm_path(path_font);
      QString elided_path = fm_path.elidedText(file_path, Qt::ElideMiddle, text_w);
      int bottom_y = top_y + fm_name.height() + 2;
      painter->drawText(QRect(text_x, bottom_y, text_w, fm_path.height()),
                       Qt::AlignLeft | Qt::AlignVCenter, elided_path);
    }

    painter->restore();
  }

  QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex & /*index*/) const override {
    return QSize(option.rect.width(), 44);
  }
};

}  // namespace

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
  setItemDelegateForColumn(SearchTableModel::kColFileName, new FileNameAndPathDelegate(this));

  horizontalHeader()->setSectionResizeMode(SearchTableModel::kColIndex, QHeaderView::Fixed);
  horizontalHeader()->setSectionResizeMode(SearchTableModel::kColFileName, QHeaderView::Stretch);
  horizontalHeader()->setSectionResizeMode(SearchTableModel::kColPath, QHeaderView::Interactive);
  horizontalHeader()->setSectionResizeMode(SearchTableModel::kColSize, QHeaderView::ResizeToContents);
  horizontalHeader()->setSectionResizeMode(SearchTableModel::kColMtime, QHeaderView::ResizeToContents);
  horizontalHeader()->setSectionResizeMode(SearchTableModel::kColScore, QHeaderView::ResizeToContents);

  setColumnWidth(SearchTableModel::kColIndex, 52);
  verticalHeader()->setVisible(false);
  verticalHeader()->setDefaultSectionSize(44);
  setIconSize(QSize(18, 18));
  setShowGrid(false);

  setColumnHidden(SearchTableModel::kColPath, true);
  setColumnHidden(SearchTableModel::kColScore, true);

  setSortingEnabled(true);
  horizontalHeader()->setSortIndicatorShown(true);
  horizontalHeader()->setSectionsClickable(true);
  horizontalHeader()->setSortIndicator(SearchTableModel::kColIndex, Qt::AscendingOrder);

  connect(horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int logical_index) {
    if (logical_index == SearchTableModel::kColIndex) {
      last_sorted_col_ = SearchTableModel::kColIndex;
      last_sort_order_ = Qt::AscendingOrder;
      horizontalHeader()->setSortIndicator(SearchTableModel::kColIndex, Qt::AscendingOrder);
      sortByColumn(SearchTableModel::kColIndex, Qt::AscendingOrder);
      return;
    }
    if (last_sorted_col_ == logical_index && last_sort_order_ == Qt::DescendingOrder) {
      // 3rd click on sorted column: cycle back to default search relevance order!
      last_sorted_col_ = SearchTableModel::kColIndex;
      last_sort_order_ = Qt::AscendingOrder;
      horizontalHeader()->setSortIndicator(SearchTableModel::kColIndex, Qt::AscendingOrder);
      sortByColumn(SearchTableModel::kColIndex, Qt::AscendingOrder);
      return;
    }
    last_sorted_col_ = logical_index;
    last_sort_order_ = horizontalHeader()->sortIndicatorOrder();
  });

  setSelectionBehavior(QAbstractItemView::SelectRows);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setEditTriggers(QAbstractItemView::NoEditTriggers);
  setAlternatingRowColors(true);
  setMouseTracking(true);

  setStyleSheet(
      "QTableView {"
      "  font-size: 13px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  alternate-background-color: #F8FAFC;"
      "  selection-background-color: #E0F2FE;"
      "  selection-color: #0369A1;"
      "  outline: none;"
      "}"
      "QTableView::item {"
      "  padding-left: 8px;"
      "  padding-right: 8px;"
      "  border: none;"
      "}"
      "QTableView::item:hover {"
      "  background-color: #F1F5F9;"
      "}"
      "QHeaderView::section {"
      "  background-color: #F8FAFC;"
      "  font-size: 12px;"
      "  color: #334155;"
      "  padding: 7px 8px;"
      "  border: none;"
      "  border-right: 1px solid #E2E8F0;"
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
  last_sorted_col_ = SearchTableModel::kColIndex;
  last_sort_order_ = Qt::AscendingOrder;
  if (horizontalHeader()) {
    horizontalHeader()->setSortIndicator(SearchTableModel::kColIndex, Qt::AscendingOrder);
  }
  if (model_) {
    model_->SetResults(items);
    if (!items.empty()) {
      selectRow(0);
    }
  }
}

void ResultView::OpenCurrentSelectedItem() {
  QModelIndex cur = currentIndex();
  if (!cur.isValid() && model_ && model_->rowCount() > 0) {
    cur = model_->index(0, SearchTableModel::kColFileName);
    setCurrentIndex(cur);
  }
  if (cur.isValid() && model_) {
    const auto *it = model_->GetItem(cur.row());
    if (it) {
      emit FileOpenRequested(it->doc_id, it->file_name, it->file_path);
    }
  }
}

void ResultView::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    OpenCurrentSelectedItem();
    event->accept();
    return;
  }
  QTableView::keyPressEvent(event);
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
