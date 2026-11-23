// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_PARSE_ISSUES_DIALOG_H_
#define SOEASY_SRC_GUI_PARSE_ISSUES_DIALOG_H_

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <memory>
#include <vector>

#include "common/parse_status.h"
#include "service/isearch_coordinator.h"

namespace soeasy {
namespace gui {

class ParseIssuesDialog : public QDialog {
  Q_OBJECT

 public:
  explicit ParseIssuesDialog(std::shared_ptr<service::ISearchCoordinator> coordinator, QWidget *parent = nullptr);
  ~ParseIssuesDialog() override = default;

  void Reload();

 private slots:
  void OnFilterChanged();
  void OnSelectionChanged();
  void OnOpenContainingFolder();
  void OnCopySelectedInfo();

 private:
  void SetupUi();
  void PopulateTable();
  [[nodiscard]] QString StatusDisplayText(common::ParseStatus status) const;

  std::shared_ptr<service::ISearchCoordinator> search_coordinator_;
  common::ParseStatusSummary summary_;
  std::vector<common::ParseIssueRecord> all_issues_;

  QLabel *kpi_ok_label_ = nullptr;
  QLabel *kpi_encrypted_label_ = nullptr;
  QLabel *kpi_timeout_label_ = nullptr;
  QLabel *kpi_crashed_label_ = nullptr;
  QLabel *kpi_failed_label_ = nullptr;

  QComboBox *status_filter_combo_ = nullptr;
  QLineEdit *keyword_filter_edit_ = nullptr;
  QPushButton *refresh_btn_ = nullptr;

  QTableWidget *table_ = nullptr;
  QPlainTextEdit *detail_box_ = nullptr;
  QPushButton *open_folder_btn_ = nullptr;
  QPushButton *copy_info_btn_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_PARSE_ISSUES_DIALOG_H_
