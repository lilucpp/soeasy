// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_SEARCH_TABLE_MODEL_H_
#define SOEASY_SRC_GUI_SEARCH_TABLE_MODEL_H_

#include <QAbstractTableModel>
#include <QString>
#include <cstdint>
#include <vector>

namespace soeasy {
namespace gui {

struct DisplayResultItem {
  int64_t doc_id = 0;
  QString file_name;
  QString file_path;
  QString ext;
  uint64_t file_size = 0;
  int64_t mtime = 0;
  double score = 0.0;
  QString snippet_html;
  bool has_body_snippet = false;
  int initial_order = 0;
};

/**
 * @brief High-performance virtual viewport table model for search results.
 *
 * Implements Qt Model/View architecture to eliminate millions of QTableWidgetItem heap allocations,
 * supporting 100K+ search result rendering with zero UI freeze.
 */
class SearchTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column {
    kColIndex = 0,
    kColFileName = 1,
    kColPath = 2,
    kColSize = 3,
    kColMtime = 4,
    kColScore = 5,
    kColCount = 6
  };

  explicit SearchTableModel(QObject *parent = nullptr);
  ~SearchTableModel() override = default;

  [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  [[nodiscard]] int columnCount(const QModelIndex &parent = QModelIndex()) const override;
  [[nodiscard]] QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  void SetResults(const std::vector<DisplayResultItem> &items);
  void AppendResults(const std::vector<DisplayResultItem> &items);
  void ClearResults();
  void RetranslateUi();
  void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

  [[nodiscard]] const DisplayResultItem *GetItem(int row) const;
  DisplayResultItem *GetItemMutable(int row);
  [[nodiscard]] const std::vector<DisplayResultItem> &items() const { return items_; }

 private:
  std::vector<DisplayResultItem> items_;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SEARCH_TABLE_MODEL_H_
