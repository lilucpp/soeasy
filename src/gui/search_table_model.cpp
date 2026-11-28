// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/search_table_model.h"

#include <QCollator>
#include <QColor>
#include <QDateTime>
#include <QFileInfo>
#include <QFont>
#include <QLocale>
#include <algorithm>
#include <cmath>
#include <optional>

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
        if (!item.container_path.isEmpty()) {
          return FormatArchiveBreadcrumb(QFileInfo(item.file_path).path(), item.container_path);
        }
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
      return FormatArchiveBreadcrumb(item.file_path, item.container_path);
    }
  }

  if (role == Qt::UserRole + 2) {
    if (col == kColFileName || col == kColPath) {
      return item.container_path;
    }
  }

  if (role == Qt::ToolTipRole) {
    if (col == kColFileName || col == kColPath) {
      return FormatArchiveBreadcrumb(item.file_path, item.container_path);
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

void SearchTableModel::RebuildDocIdIndex() {
  doc_id_to_row_.clear();
  doc_id_to_row_.reserve(items_.size());
  for (size_t i = 0; i < items_.size(); ++i) {
    doc_id_to_row_[items_[i].doc_id] = i;
  }
}

void SearchTableModel::SetResults(const std::vector<DisplayResultItem> &items) {
  beginResetModel();
  items_ = items;
  for (size_t i = 0; i < items_.size(); ++i) {
    items_[i].initial_order = static_cast<int>(i);
  }
  RebuildDocIdIndex();
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
    doc_id_to_row_[items_[i].doc_id] = i;
  }
  endInsertRows();
}

bool SearchTableModel::UpdateSnippet(int64_t doc_id, const QString &snippet_html) {
  auto it = doc_id_to_row_.find(doc_id);
  if (it == doc_id_to_row_.end() || it->second >= items_.size()) {
    return false;
  }
  DisplayResultItem &item = items_[it->second];
  if (item.doc_id != doc_id) {
    return false;
  }
  item.snippet_html = snippet_html;
  item.has_body_snippet = true;
  return true;
}

void SearchTableModel::ClearResults() {
  beginResetModel();
  items_.clear();
  doc_id_to_row_.clear();
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

  const QModelIndexList persistent = persistentIndexList();
  std::vector<int64_t> persistent_doc_ids;
  persistent_doc_ids.reserve(persistent.size());
  for (const QModelIndex &idx : persistent) {
    if (idx.isValid() && idx.row() >= 0 && idx.row() < static_cast<int>(items_.size())) {
      persistent_doc_ids.push_back(items_[idx.row()].doc_id);
    } else {
      persistent_doc_ids.push_back(-1);
    }
  }

  std::optional<QCollator> collator;
  if (column == kColFileName || column == kColPath) {
    collator.emplace(QLocale(QLocale::Chinese, QLocale::China));
    collator->setNumericMode(true);
    collator->setCaseSensitivity(Qt::CaseInsensitive);
  }

  auto is_less = [column, &collator](const DisplayResultItem &a, const DisplayResultItem &b) -> bool {
    switch (column) {
      case kColFileName:
        return collator->compare(a.file_name, b.file_name) < 0;
      case kColPath:
        return collator->compare(a.file_path, b.file_path) < 0;
      case kColSize:
        return a.file_size < b.file_size;
      case kColMtime:
        return a.mtime < b.mtime;
      case kColScore: {
        const bool a_nan = std::isnan(a.score);
        const bool b_nan = std::isnan(b.score);
        if (a_nan || b_nan) {
          return !a_nan && b_nan;
        }
        return a.score < b.score;
      }
      case kColIndex:
      default:
        return a.initial_order < b.initial_order;
    }
  };

  std::stable_sort(items_.begin(), items_.end(), [&is_less, order](const DisplayResultItem &a, const DisplayResultItem &b) {
    return (order == Qt::AscendingOrder) ? is_less(a, b) : is_less(b, a);
  });

  RebuildDocIdIndex();

  if (!persistent.isEmpty()) {
    QModelIndexList updated;
    updated.reserve(persistent.size());
    for (int i = 0; i < persistent.size(); ++i) {
      const QModelIndex &old_idx = persistent[i];
      const int64_t doc_id = persistent_doc_ids[static_cast<size_t>(i)];
      auto it = (doc_id >= 0) ? doc_id_to_row_.find(doc_id) : doc_id_to_row_.end();
      if (it != doc_id_to_row_.end()) {
        updated.push_back(index(static_cast<int>(it->second), old_idx.column(), old_idx.parent()));
      } else {
        updated.push_back(QModelIndex());
      }
    }
    changePersistentIndexList(persistent, updated);
  }

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
