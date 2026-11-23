// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/parse_issues_dialog.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include "gui/gui_utils.h"
#include "gui/i18n.h"

namespace soeasy {
namespace gui {

namespace {

constexpr int kColStatus = 0;
constexpr int kColName = 1;
constexpr int kColExt = 2;
constexpr int kColSize = 3;
constexpr int kColPath = 4;
constexpr int kColReason = 5;

constexpr int kRoleIssueIndex = Qt::UserRole;

}  // namespace

ParseIssuesDialog::ParseIssuesDialog(std::shared_ptr<service::ISearchCoordinator> coordinator, QWidget *parent)
    : QDialog(parent), search_coordinator_(std::move(coordinator)) {
  setWindowTitle(Tr("issues_dlg_title"));
  resize(960, 620);
  setMinimumSize(720, 460);
  SetupUi();
  Reload();
}

void ParseIssuesDialog::SetupUi() {
  setStyleSheet(
      "QDialog { background-color: #FFFAFA; color: #0F172A; }"
      "QTableWidget {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  gridline-color: #F1F5F9;"
      "  selection-background-color: #E0F2FE;"
      "  selection-color: #0F172A;"
      "  font-size: 12px;"
      "}"
      "QHeaderView::section {"
      "  background-color: #F8FAFC;"
      "  color: #334155;"
      "  border: none;"
      "  border-bottom: 1px solid #CBD5E1;"
      "  border-right: 1px solid #E2E8F0;"
      "  padding: 6px 8px;"
      "  font-weight: bold;"
      "}"
      "QLineEdit, QComboBox {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 5px;"
      "  padding: 4px 8px;"
      "  font-size: 12px;"
      "}"
      "QPlainTextEdit {"
      "  background-color: #F8FAFC;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  color: #334155;"
      "  font-size: 12px;"
      "  padding: 4px;"
      "}");

  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(14, 12, 14, 12);
  main_layout->setSpacing(10);

  // 1. Summary KPI Row
  auto *summary_row = new QHBoxLayout();
  summary_row->setSpacing(8);

  auto make_kpi = [this](const QString &bg, const QString &fg, const QString &border) {
    auto *lbl = new QLabel(this);
    lbl->setStyleSheet(QString("background-color: %1; color: %2; border: 1px solid %3; "
                               "border-radius: 6px; padding: 5px 10px; font-size: 12px; font-weight: bold;")
                           .arg(bg, fg, border));
    return lbl;
  };

  kpi_ok_label_ = make_kpi("#DCFCE7", "#15803D", "#BBF7D0");
  kpi_encrypted_label_ = make_kpi("#FEF3C7", "#B45309", "#FDE68A");
  kpi_timeout_label_ = make_kpi("#FFEDD5", "#C2410C", "#FED7AA");
  kpi_crashed_label_ = make_kpi("#FEE2E2", "#B91C1C", "#FECACA");
  kpi_failed_label_ = make_kpi("#FEE2E2", "#DC2626", "#FCA5A5");

  summary_row->addWidget(kpi_ok_label_);
  summary_row->addWidget(kpi_encrypted_label_);
  summary_row->addWidget(kpi_timeout_label_);
  summary_row->addWidget(kpi_crashed_label_);
  summary_row->addWidget(kpi_failed_label_);
  summary_row->addStretch(1);

  refresh_btn_ = new QPushButton(Tr("issues_btn_refresh"), this);
  refresh_btn_->setCursor(Qt::PointingHandCursor);
  connect(refresh_btn_, &QPushButton::clicked, this, &ParseIssuesDialog::Reload);
  summary_row->addWidget(refresh_btn_);
  main_layout->addLayout(summary_row);

  // 2. Filter Row
  auto *filter_row = new QHBoxLayout();
  filter_row->setSpacing(8);

  status_filter_combo_ = new QComboBox(this);
  status_filter_combo_->addItem(Tr("issues_filter_all"), -1);
  status_filter_combo_->addItem(Tr("issues_status_encrypted"), static_cast<int>(common::ParseStatus::kEncrypted));
  status_filter_combo_->addItem(Tr("issues_status_failed"), static_cast<int>(common::ParseStatus::kFailed));
  status_filter_combo_->addItem(Tr("issues_status_timeout"), static_cast<int>(common::ParseStatus::kTimeout));
  status_filter_combo_->addItem(Tr("issues_status_crashed"), static_cast<int>(common::ParseStatus::kCrashed));
  connect(status_filter_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &ParseIssuesDialog::OnFilterChanged);

  keyword_filter_edit_ = new QLineEdit(this);
  keyword_filter_edit_->setPlaceholderText(Tr("issues_filter_ph"));
  keyword_filter_edit_->setClearButtonEnabled(true);
  connect(keyword_filter_edit_, &QLineEdit::textChanged, this, &ParseIssuesDialog::OnFilterChanged);

  filter_row->addWidget(new QLabel(Tr("issues_filter_label"), this));
  filter_row->addWidget(status_filter_combo_);
  filter_row->addWidget(keyword_filter_edit_, 1);
  main_layout->addLayout(filter_row);

  // 3. Issues Table
  table_ = new QTableWidget(0, 6, this);
  table_->setHorizontalHeaderLabels({Tr("issues_col_status"), Tr("issues_col_name"), Tr("issues_col_ext"),
                                     Tr("issues_col_size"), Tr("issues_col_path"), Tr("issues_col_reason")});
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->verticalHeader()->setVisible(false);
  table_->setWordWrap(false);
  table_->horizontalHeader()->setSectionResizeMode(kColStatus, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(kColName, QHeaderView::Interactive);
  table_->horizontalHeader()->setSectionResizeMode(kColExt, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(kColSize, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(kColPath, QHeaderView::Interactive);
  table_->horizontalHeader()->setSectionResizeMode(kColReason, QHeaderView::Stretch);
  table_->setColumnWidth(kColName, 170);
  table_->setColumnWidth(kColPath, 260);
  connect(table_, &QTableWidget::itemSelectionChanged, this, &ParseIssuesDialog::OnSelectionChanged);
  connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { OnOpenContainingFolder(); });
  main_layout->addWidget(table_, 1);

  // 4. Full Detail Box
  detail_box_ = new QPlainTextEdit(this);
  detail_box_->setReadOnly(true);
  detail_box_->setFixedHeight(78);
  detail_box_->setPlaceholderText(Tr("issues_detail_ph"));
  main_layout->addWidget(detail_box_);

  // 5. Bottom Action Row
  auto *btn_row = new QHBoxLayout();
  open_folder_btn_ = new QPushButton(Tr("btn_open_folder_sys"), this);
  copy_info_btn_ = new QPushButton(Tr("issues_btn_copy"), this);
  auto *close_btn = new QPushButton(Tr("close"), this);
  open_folder_btn_->setEnabled(false);
  copy_info_btn_->setEnabled(false);

  connect(open_folder_btn_, &QPushButton::clicked, this, &ParseIssuesDialog::OnOpenContainingFolder);
  connect(copy_info_btn_, &QPushButton::clicked, this, &ParseIssuesDialog::OnCopySelectedInfo);
  connect(close_btn, &QPushButton::clicked, this, &QDialog::accept);

  btn_row->addWidget(open_folder_btn_);
  btn_row->addWidget(copy_info_btn_);
  btn_row->addStretch(1);
  btn_row->addWidget(close_btn);
  main_layout->addLayout(btn_row);
}

QString ParseIssuesDialog::StatusDisplayText(common::ParseStatus status) const {
  switch (status) {
    case common::ParseStatus::kEncrypted:
      return Tr("issues_status_encrypted");
    case common::ParseStatus::kTimeout:
      return Tr("issues_status_timeout");
    case common::ParseStatus::kCrashed:
      return Tr("issues_status_crashed");
    case common::ParseStatus::kFailed:
    default:
      return Tr("issues_status_failed");
  }
}

void ParseIssuesDialog::Reload() {
  if (search_coordinator_) {
    summary_ = search_coordinator_->GetParseStatusSummary();
    all_issues_ = search_coordinator_->GetParseIssues(std::nullopt, 5000, 0);
  } else {
    summary_ = {};
    all_issues_.clear();
  }

  kpi_ok_label_->setText(Tr("issues_kpi_ok").arg(summary_.ok_count));
  kpi_encrypted_label_->setText(Tr("issues_kpi_encrypted").arg(summary_.encrypted_count));
  kpi_timeout_label_->setText(Tr("issues_kpi_timeout").arg(summary_.timeout_count));
  kpi_crashed_label_->setText(Tr("issues_kpi_crashed").arg(summary_.crashed_count));
  kpi_failed_label_->setText(Tr("issues_kpi_failed").arg(summary_.failed_count));
  if (all_issues_.size() < summary_.issue_count()) {
    setWindowTitle(Tr("issues_dlg_title_truncated").arg(all_issues_.size()).arg(summary_.issue_count()));
  } else {
    setWindowTitle(Tr("issues_dlg_title"));
  }

  PopulateTable();
}

void ParseIssuesDialog::PopulateTable() {
  const int filter_stat = status_filter_combo_->currentData().toInt();
  const QString kw = keyword_filter_edit_->text().trimmed();

  table_->setSortingEnabled(false);
  table_->setRowCount(0);

  for (size_t i = 0; i < all_issues_.size(); ++i) {
    const auto &rec = all_issues_[i];
    if (filter_stat >= 0 && static_cast<int>(rec.status) != filter_stat) {
      continue;
    }

    const QString q_name = QString::fromStdString(rec.file_name);
    const QString q_path = QString::fromStdString(rec.file_path);
    const QString q_reason = QString::fromStdString(rec.reason);
    const QString q_ext = QString::fromStdString(rec.ext);

    if (!kw.isEmpty() && !q_name.contains(kw, Qt::CaseInsensitive) && !q_path.contains(kw, Qt::CaseInsensitive) &&
        !q_reason.contains(kw, Qt::CaseInsensitive) && !q_ext.contains(kw, Qt::CaseInsensitive)) {
      continue;
    }

    const int row = table_->rowCount();
    table_->insertRow(row);

    auto *status_item = new QTableWidgetItem(StatusDisplayText(rec.status));
    status_item->setData(kRoleIssueIndex, static_cast<qulonglong>(i));
    status_item->setTextAlignment(Qt::AlignCenter);
    if (rec.status == common::ParseStatus::kEncrypted) {
      status_item->setForeground(QColor("#B45309"));
    } else if (rec.status == common::ParseStatus::kTimeout) {
      status_item->setForeground(QColor("#C2410C"));
    } else {
      status_item->setForeground(QColor("#DC2626"));
    }

    auto *name_item = new QTableWidgetItem(q_name);
    auto *ext_item = new QTableWidgetItem(q_ext);
    ext_item->setTextAlignment(Qt::AlignCenter);
    auto *size_item = new QTableWidgetItem(FormatFileSize(rec.file_size));
    size_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *path_item = new QTableWidgetItem(q_path);
    path_item->setToolTip(q_path);
    auto *reason_item = new QTableWidgetItem(q_reason);
    reason_item->setToolTip(q_reason);

    table_->setItem(row, kColStatus, status_item);
    table_->setItem(row, kColName, name_item);
    table_->setItem(row, kColExt, ext_item);
    table_->setItem(row, kColSize, size_item);
    table_->setItem(row, kColPath, path_item);
    table_->setItem(row, kColReason, reason_item);
  }

  table_->setSortingEnabled(true);
  OnSelectionChanged();
}

void ParseIssuesDialog::OnFilterChanged() {
  PopulateTable();
}

void ParseIssuesDialog::OnSelectionChanged() {
  const auto items = table_->selectedItems();
  if (items.isEmpty()) {
    detail_box_->clear();
    open_folder_btn_->setEnabled(false);
    copy_info_btn_->setEnabled(false);
    return;
  }

  const int row = items.first()->row();
  auto *status_item = table_->item(row, kColStatus);
  if (!status_item) return;

  const size_t idx = static_cast<size_t>(status_item->data(kRoleIssueIndex).toULongLong());
  if (idx >= all_issues_.size()) return;

  const auto &rec = all_issues_[idx];
  const QString detail = QString("[%1] %2\n%3: %4\n%5: %6")
                             .arg(StatusDisplayText(rec.status), QString::fromStdString(rec.file_name),
                                  Tr("issues_col_path"), QString::fromStdString(rec.file_path), Tr("issues_col_reason"),
                                  QString::fromStdString(rec.reason.empty() ? "-" : rec.reason));
  detail_box_->setPlainText(detail);
  open_folder_btn_->setEnabled(!rec.file_path.empty());
  copy_info_btn_->setEnabled(true);
}

void ParseIssuesDialog::OnOpenContainingFolder() {
  const int row = table_->currentRow();
  if (row < 0) return;
  auto *path_item = table_->item(row, kColPath);
  if (!path_item) return;
  const QString path = path_item->text();
  if (path.isEmpty()) return;
  QFileInfo fi(path);
  QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

void ParseIssuesDialog::OnCopySelectedInfo() {
  const QString text = detail_box_->toPlainText();
  if (!text.isEmpty() && QApplication::clipboard()) {
    QApplication::clipboard()->setText(text);
  }
}

}  // namespace gui
}  // namespace soeasy
