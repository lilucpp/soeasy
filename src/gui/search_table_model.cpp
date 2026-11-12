// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/search_table_model.h"

#include <algorithm>
#include <QColor>
#include <QDateTime>
#include <QFileInfo>
#include <QFont>

#include "gui/gui_utils.h"
#include "gui/i18n.h"
#include "gui/icon_provider.h"

namespace soeasy {
namespace gui {

SearchTableModel::SearchTableModel(QObject *parent) : QAbstractTableModel(parent) {}

int SearchTableModel::rowCount(const QModelIndex &parent) const {
  if (parent.isValid()) {
    return 0;
  }
  return static_cast<int>(items_.size());
}

int SearchTableModel::columnCount(const QModelIndex &parent) const {
  if (parent.isValid()) {
    return 0;
  }
  return kColCount;
}

QVariant SearchTableModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(items_.size())) {
    return QVariant();
  }

  const auto &item = items_[index.row()];
  int col = index.column();

  if (role == Qt::DisplayRole) {
    switch (col) {
      case kColIndex:
        return index.row() + 1;
      case kColFileName:
        return item.file_name;
      case kColPath:
        return QFileInfo(item.file_path).absolutePath();
      case kColSize:
        return FormatFileSize(item.file_size);
      case kColMtime:
        return QDateTime::fromSecsSinceEpoch(item.mtime).toString("yyyy-MM-dd hh:mm");
      case kColScore:
        if (item.score <= 0.0) {
          return QString("-");
        }
        return QString::number(item.score, 'f', 1);
      default:
        return QVariant();
    }
  }

  if (role == Qt::DecorationRole && col == kColFileName) {
    QString ext = item.ext.isEmpty() ? QFileInfo(item.file_path).suffix() : item.ext;
    return IconProvider::GetFileIcon(ext);
  }

  if (role == Qt::UserRole + 1) {
    if (col == kColFileName || col == kColPath) {
      return item.file_path;
    }
  }

  if (role == Qt::ToolTipRole) {
    if (col == kColFileName || col == kColPath) {
      return item.file_path;
    }
  }

  if (role == Qt::TextAlignmentRole) {
    switch (col) {
      case kColIndex:
        return Qt::AlignCenter;
      case kColSize:
      case kColScore:
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
      case kColMtime:
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
      default:
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }
  }

  if (role == Qt::ForegroundRole) {
    switch (col) {
      case kColIndex:
        return QColor("#94A3B8");
      case kColPath:
        return QColor("#64748B");
      default:
        return QVariant();
    }
  }

  return QVariant();
}

QVariant SearchTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation == Qt::Horizontal) {
    if (role == Qt::DisplayRole) {
      switch (section) {
        case kColIndex:
          return Tr("th_id");
        case kColFileName:
          return Tr("th_filename");
        case kColPath:
          return Tr("th_path");
        case kColSize:
          return Tr("th_size");
        case kColMtime:
          return Tr("th_mtime");
        case kColScore:
          return Tr("th_score");
        default:
          return QVariant();
      }
    }
    if (role == Qt::TextAlignmentRole) {
      return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (role == Qt::ToolTipRole) {
      if (section == kColIndex) {
        return Tr("tip_header_id");
      }
    }
  }
  return QVariant();
}

void SearchTableModel::SetResults(const std::vector<DisplayResultItem> &items) {
  beginResetModel();
  items_ = items;
  for (size_t i = 0; i < items_.size(); ++i) {
    items_[i].initial_order = static_cast<int>(i);
  }
  endResetModel();
}

void SearchTableModel::AppendResults(const std::vector<DisplayResultItem> &items) {
  if (items.empty()) {
    return;
  }
  int first = static_cast<int>(items_.size());
  int last = first + static_cast<int>(items.size()) - 1;
  beginInsertRows(QModelIndex(), first, last);
  size_t start_idx = items_.size();
  items_.insert(items_.end(), items.begin(), items.end());
  for (size_t i = start_idx; i < items_.size(); ++i) {
    items_[i].initial_order = static_cast<int>(i);
  }
  endInsertRows();
}

void SearchTableModel::ClearResults() {
  beginResetModel();
  items_.clear();
  endResetModel();
}

void SearchTableModel::RetranslateUi() {
  emit headerDataChanged(Qt::Horizontal, 0, kColCount - 1);
}

void SearchTableModel::sort(int column, Qt::SortOrder order) {
  if (items_.empty()) {
    return;
  }
  layoutAboutToBeChanged();

  std::sort(items_.begin(), items_.end(), [column, order](const DisplayResultItem &a, const DisplayResultItem &b) {
    bool less = false;
    switch (column) {
      case kColFileName:
        less = a.file_name.compare(b.file_name, Qt::CaseInsensitive) < 0;
        break;
      case kColPath:
        less = a.file_path.compare(b.file_path, Qt::CaseInsensitive) < 0;
        break;
      case kColSize:
        less = a.file_size < b.file_size;
        break;
      case kColMtime:
        less = a.mtime < b.mtime;
        break;
      case kColScore:
        // Score: negative values, smaller is more relevant in SQLite FTS5
        less = a.score < b.score;
        break;
      case kColIndex:
      default:
        less = a.initial_order < b.initial_order;
        break;
    }
    return (order == Qt::AscendingOrder) ? less : !less;
  });

  layoutChanged();
}

const DisplayResultItem *SearchTableModel::GetItem(int row) const {
  if (row >= 0 && row < static_cast<int>(items_.size())) {
    return &items_[row];
  }
  return nullptr;
}

DisplayResultItem *SearchTableModel::GetItemMutable(int row) {
  if (row >= 0 && row < static_cast<int>(items_.size())) {
    return &items_[row];
  }
  return nullptr;
}

}  // namespace gui
}  // namespace soeasy
