// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/content_preview_dialog.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "gui/gui_utils.h"
#include "gui/i18n.h"
#include "gui/icon_provider.h"

namespace soeasy {
namespace gui {

ContentPreviewDialog::ContentPreviewDialog(int64_t doc_id, const QString &file_name, const QString &file_path,
                                           const std::vector<std::string> &keywords,
                                           std::shared_ptr<storage::StorageEngine> storage, QWidget *parent)
    : QDialog(parent),
      doc_id_(doc_id),
      file_name_(file_name),
      file_path_(file_path),
      keywords_(keywords),
      storage_(std::move(storage)) {
  SetupUi();
  LoadContentAndHighlight();
}

void ContentPreviewDialog::SetupUi() {
  setWindowTitle(Tr("preview_dlg_title").arg(file_name_));
  resize(880, 680);
  setStyleSheet("QDialog { background-color: #F8FAFC; }");

  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(16, 16, 16, 16);
  main_layout->setSpacing(10);

  // 1. Top Card (File Info & Direct Actions)
  auto *header_card = new QWidget(this);
  header_card->setStyleSheet(
      "background-color: #FFFFFF; border: 1px solid #E2E8F0; border-radius: 6px; padding: 10px;");
  auto *header_layout = new QVBoxLayout(header_card);
  header_layout->setContentsMargins(10, 10, 10, 10);
  header_layout->setSpacing(4);

  auto *title_row = new QHBoxLayout();
  auto *icon_label = new QLabel(header_card);
  QFileInfo fi(file_path_);
  icon_label->setPixmap(IconProvider::GetFileIcon(fi.suffix()).pixmap(24, 24));
  icon_label->setStyleSheet("border: none;");

  auto *title_label = new QLabel(file_name_, header_card);
  title_label->setStyleSheet("font-size: 14px; font-weight: bold; color: #0F172A; border: none;");
  title_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto *btn_open_file = new QPushButton(Tr("btn_open_file"), header_card);
  btn_open_file->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px; padding: 4px 10px; border: 1px solid #CBD5E1; border-radius: 4px; background-color: #F8FAFC; color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }");

  auto *btn_open_folder = new QPushButton(Tr("btn_open_folder"), header_card);
  btn_open_folder->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px; padding: 4px 10px; border: 1px solid #CBD5E1; border-radius: 4px; background-color: #F8FAFC; color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }");

  title_row->addWidget(icon_label);
  title_row->addWidget(title_label, 1);
  title_row->addWidget(btn_open_file);
  title_row->addWidget(btn_open_folder);
  header_layout->addLayout(title_row);

  path_label_ = new QLabel(file_path_, header_card);
  path_label_->setStyleSheet("font-size: 11px; color: #64748B; border: none; font-family: monospace;");
  path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  path_label_->setWordWrap(true);
  header_layout->addWidget(path_label_);

  main_layout->addWidget(header_card);

  // 2. Reusable Document Viewer
  viewer_ = new DocumentViewerWidget(this);
  main_layout->addWidget(viewer_, 1);

  connect(btn_open_file, &QPushButton::clicked, this, &ContentPreviewDialog::OnOpenWithDefaultApp);
  connect(btn_open_folder, &QPushButton::clicked, this, &ContentPreviewDialog::OnOpenFolder);
}

void ContentPreviewDialog::LoadContentAndHighlight() {
  if (!viewer_) return;

  std::string decompressed_utf8;
  bool ok = DocumentViewerWidget::ReadDocumentContent(doc_id_, file_path_, storage_, &decompressed_utf8);

  if (!ok || decompressed_utf8.empty()) {
    viewer_->SetHtml(QString("<p style='color:#94A3B8; font-style:italic;'>%1</p>").arg(Tr("preview_cannot_read")));
    return;
  }

  viewer_->LoadContent(QString::fromStdString(decompressed_utf8), keywords_);
}

void ContentPreviewDialog::OnOpenFolder() {
  if (file_path_.isEmpty()) return;
  QFileInfo fi(file_path_);
  QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

void ContentPreviewDialog::OnOpenWithDefaultApp() {
  if (file_path_.isEmpty()) return;
  QDesktopServices::openUrl(QUrl::fromLocalFile(file_path_));
}

}  // namespace gui
}  // namespace soeasy
