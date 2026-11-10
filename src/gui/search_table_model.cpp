// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/search_table_model.h"

#include <QColor>
#include <QDateTime>
#include <QFileInfo>
#include <QFont>

#include "gui/i18n.h"
#include "gui/icon_provider.h"

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
        return QString::number(item.score, 'f', 2);
      default:
        return QVariant();
    }
  }

  if (role == Qt::DecorationRole && col == kColFileName) {
    QString ext = item.ext.isEmpty() ? QFileInfo(item.file_path).suffix() : item.ext;
    return IconProvider::GetFileIcon(ext);
  }

  if (role == Qt::TextAlignmentRole) {
    switch (col) {
      case kColIndex:
      case kColMtime:
        return Qt::AlignCenter;
      case kColSize:
      case kColScore:
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
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
          return Tr("th_filename_path").isEmpty() || Tr("th_filename_path") == "th_filename_path"
                     ? "文件名 / 所在路径"
                     : Tr("th_filename_path");
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
      switch (section) {
        case kColIndex:
        case kColMtime:
          return Qt::AlignCenter;
        case kColSize:
        case kColScore:
          return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        default:
          return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
      }
    }
  }
  return QVariant();
}

void SearchTableModel::SetResults(const std::vector<DisplayResultItem> &items) {
  beginResetModel();
  items_ = items;
  endResetModel();
}

void SearchTableModel::AppendResults(const std::vector<DisplayResultItem> &items) {
  if (items.empty()) {
    return;
  }
  int first = static_cast<int>(items_.size());
  int last = first + static_cast<int>(items.size()) - 1;
  beginInsertRows(QModelIndex(), first, last);
  items_.insert(items_.end(), items.begin(), items.end());
  endInsertRows();
}

void SearchTableModel::ClearResults() {
  beginResetModel();
  items_.clear();
  endResetModel();
}

void SearchTableModel::SortResults(int type) {
  if (items_.size() <= 1) return;
  beginResetModel();
  if (type == 1) {  // Modified time descending
    std::sort(items_.begin(), items_.end(), [](const DisplayResultItem &a, const DisplayResultItem &b) {
      return a.mtime > b.mtime;
    });
  } else if (type == 2) {  // File size descending
    std::sort(items_.begin(), items_.end(), [](const DisplayResultItem &a, const DisplayResultItem &b) {
      return a.file_size > b.file_size;
    });
  } else if (type == 3) {  // File name ascending
    std::sort(items_.begin(), items_.end(), [](const DisplayResultItem &a, const DisplayResultItem &b) {
      return a.file_name.compare(b.file_name, Qt::CaseInsensitive) < 0;
    });
  } else {  // Score descending
    std::sort(items_.begin(), items_.end(), [](const DisplayResultItem &a, const DisplayResultItem &b) {
      return a.score > b.score;
    });
  }
  endResetModel();
}

void SearchTableModel::RetranslateUi() {
  emit headerDataChanged(Qt::Horizontal, 0, kColCount - 1);
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
