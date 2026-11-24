// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/index_progress_dialog.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QVBoxLayout>
#include <algorithm>

#include "gui/i18n.h"

namespace soeasy {
namespace gui {

namespace {

QString FormatWorkerElapsedMs(uint64_t elapsed_ms) {
  if (elapsed_ms < 1000) {
    return QString("%1 ms").arg(elapsed_ms);
  }
  const double secs = static_cast<double>(elapsed_ms) / 1000.0;
  if (secs < 60.0) {
    return QString("%1 s").arg(secs, 0, 'f', 1);
  }
  const uint64_t total_secs = elapsed_ms / 1000;
  const uint64_t mins = total_secs / 60;
  const uint64_t rem_secs = total_secs % 60;
  return QString("%1m %2s").arg(mins).arg(rem_secs);
}

QString TaskKindPrefix(service::IndexTaskKind kind) {
  if (kind == service::IndexTaskKind::kRebuild) {
    return Tr("status_badge_rebuild");
  }
  if (kind == service::IndexTaskKind::kInitial) {
    return Tr("status_badge_initial");
  }
  return Tr("status_badge_incremental");
}

}  // namespace

IndexProgressDialog::IndexProgressDialog(std::shared_ptr<service::IndexPipeline> pipeline, QWidget *parent)
    : QDialog(parent), pipeline_(std::move(pipeline)) {
  setWindowTitle(Tr("progress_dlg_title"));
  setMinimumSize(680, 340);
  resize(740, 380);
  SetupUi();

  refresh_timer_ = new QTimer(this);
  refresh_timer_->setInterval(200);
  connect(refresh_timer_, &QTimer::timeout, this, &IndexProgressDialog::Refresh);
}

void IndexProgressDialog::SetupUi() {
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
      "}");

  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(14, 12, 14, 12);
  main_layout->setSpacing(10);

  summary_label_ = new QLabel(this);
  summary_label_->setTextFormat(Qt::RichText);
  summary_label_->setStyleSheet("color: #0F172A; font-size: 13px; font-weight: 500;");
  main_layout->addWidget(summary_label_);

  progress_bar_ = new QProgressBar(this);
  progress_bar_->setRange(0, 100);
  progress_bar_->setValue(0);
  progress_bar_->setTextVisible(true);
  progress_bar_->setFixedHeight(18);
  progress_bar_->setStyleSheet(
      "QProgressBar {"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 5px;"
      "  background-color: #E2E8F0;"
      "  text-align: center;"
      "  font-size: 11px;"
      "  color: #0F172A;"
      "}"
      "QProgressBar::chunk {"
      "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0EA5E9, stop:1 #38BDF8);"
      "  border-radius: 4px;"
      "}");
  main_layout->addWidget(progress_bar_);

  worker_table_ = new QTableWidget(0, 3, this);
  worker_table_->setHorizontalHeaderLabels(
      {Tr("progress_col_worker"), Tr("progress_col_status"), Tr("progress_col_file")});
  worker_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  worker_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  worker_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  worker_table_->setAlternatingRowColors(true);
  worker_table_->setTextElideMode(Qt::ElideMiddle);
  worker_table_->verticalHeader()->setVisible(false);
  worker_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  worker_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  worker_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  main_layout->addWidget(worker_table_, 1);

  auto *btn_row = new QHBoxLayout();
  cancel_btn_ = new QPushButton(Tr("btn_cancel_index"), this);
  cancel_btn_->setCursor(Qt::PointingHandCursor);
  connect(cancel_btn_, &QPushButton::clicked, this, [this]() {
    if (pipeline_) {
      pipeline_->CancelScan();
    }
    SyncCancelButtonState();
    emit CancelRequested();
  });

  close_btn_ = new QPushButton(Tr("close"), this);
  close_btn_->setCursor(Qt::PointingHandCursor);
  connect(close_btn_, &QPushButton::clicked, this, &QDialog::close);

  btn_row->addWidget(cancel_btn_);
  btn_row->addStretch(1);
  btn_row->addWidget(close_btn_);
  main_layout->addLayout(btn_row);
}

void IndexProgressDialog::SyncCancelButtonState() {
  if (!cancel_btn_) {
    return;
  }
  const bool cancelled = (pipeline_ && pipeline_->is_scan_cancelled());
  cancel_btn_->setEnabled(!cancelled);
  cancel_btn_->setText(Tr(cancelled ? "btn_cancelling_index" : "btn_cancel_index"));
}

void IndexProgressDialog::RetranslateUi() {
  setWindowTitle(Tr("progress_dlg_title"));
  if (worker_table_) {
    worker_table_->setHorizontalHeaderLabels(
        {Tr("progress_col_worker"), Tr("progress_col_status"), Tr("progress_col_file")});
  }
  SyncCancelButtonState();
  if (close_btn_) {
    close_btn_->setText(Tr("close"));
  }
  Refresh();
}

void IndexProgressDialog::StartTracking(int fallback_task_type) {
  fallback_task_type_ = fallback_task_type;
  saw_active_scan_ = false;
  SyncCancelButtonState();
  if (isVisible()) {
    Refresh();
  }
  if (refresh_timer_ && !refresh_timer_->isActive()) {
    refresh_timer_->start();
  }
}

void IndexProgressDialog::HandleTaskFinished() {
  if (refresh_timer_) {
    refresh_timer_->stop();
  }
  close();
}

void IndexProgressDialog::showEvent(QShowEvent *event) {
  QDialog::showEvent(event);
  Refresh();
  if (refresh_timer_ && !refresh_timer_->isActive()) {
    refresh_timer_->start();
  }
}

void IndexProgressDialog::hideEvent(QHideEvent *event) {
  if (refresh_timer_) {
    refresh_timer_->stop();
  }
  QDialog::hideEvent(event);
}

void IndexProgressDialog::closeEvent(QCloseEvent *event) {
  if (refresh_timer_) {
    refresh_timer_->stop();
  }
  QDialog::closeEvent(event);
}

void IndexProgressDialog::Refresh() {
  SyncCancelButtonState();

  const service::ActiveScanSnapshot snap =
      pipeline_ ? pipeline_->GetActiveScanSnapshot() : service::ActiveScanSnapshot{};

  if (!snap.active) {
    if (!saw_active_scan_) {
      const QString badge = TaskKindPrefix(static_cast<service::IndexTaskKind>(fallback_task_type_));
      summary_label_->setText(badge + Tr("progress_dlg_preparing"));
      progress_bar_->setValue(0);
      worker_table_->setRowCount(0);
    }
    return;
  }
  saw_active_scan_ = true;

  const QString badge = TaskKindPrefix(snap.task_kind);
  const int pct =
      (snap.total_files > 0)
          ? static_cast<int>(std::clamp<uint64_t>((static_cast<uint64_t>(snap.processed_count) * 100) / snap.total_files,
                                                  0, 100))
          : 0;
  progress_bar_->setValue(pct);

  const QString elapsed_str = FormatWorkerElapsedMs(snap.scan_elapsed_ms);
  summary_label_->setText(badge + Tr("progress_dlg_summary")
                                      .arg(snap.indexed_count)
                                      .arg(snap.processed_count)
                                      .arg(snap.total_files)
                                      .arg(pct)
                                      .arg(elapsed_str));

  const int row_count = static_cast<int>(snap.workers.size());
  if (worker_table_->rowCount() != row_count) {
    worker_table_->setRowCount(row_count);
    for (int r = 0; r < row_count; ++r) {
      for (int c = 0; c < 3; ++c) {
        if (!worker_table_->item(r, c)) {
          worker_table_->setItem(r, c, new QTableWidgetItem());
        }
      }
    }
  }

  for (int r = 0; r < row_count; ++r) {
    const auto &ws = snap.workers[static_cast<size_t>(r)];
    auto *worker_item = worker_table_->item(r, 0);
    auto *status_item = worker_table_->item(r, 1);
    auto *file_item = worker_table_->item(r, 2);

    worker_item->setText(QString("Worker #%1").arg(ws.worker_index + 1));
    worker_item->setTextAlignment(Qt::AlignCenter);

    if (ws.active) {
      status_item->setText(Tr("progress_worker_busy").arg(FormatWorkerElapsedMs(ws.elapsed_ms)));
      status_item->setForeground(QColor("#0284C7"));
      const QString qpath = QString::fromStdString(ws.file_path);
      file_item->setText(qpath);
      file_item->setToolTip(qpath);
      file_item->setForeground(QColor("#0F172A"));
    } else {
      status_item->setText(Tr("progress_worker_idle"));
      status_item->setForeground(QColor("#64748B"));
      file_item->setText("-");
      file_item->setToolTip(QString());
      file_item->setForeground(QColor("#94A3B8"));
    }
    status_item->setTextAlignment(Qt::AlignCenter);
  }
}

}  // namespace gui
}  // namespace soeasy
