// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/content_preview_dialog.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <utility>

#include "gui/i18n.h"
#include "gui/icon_provider.h"

namespace soeasy {
namespace gui {

ContentPreviewDialog::ContentPreviewDialog(int64_t doc_id, QString file_name, QString file_path,
                                           std::vector<std::string> keywords,
                                           std::shared_ptr<service::ISearchCoordinator> coordinator, QWidget *parent,
                                           QString container_path)
    : QDialog(parent),
      doc_id_(doc_id),
      file_name_(std::move(file_name)),
      file_path_(std::move(file_path)),
      container_path_(std::move(container_path)),
      keywords_(std::move(keywords)),
      search_coordinator_(std::move(coordinator)) {
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

  auto *title_label = new ElidedLabel(Qt::ElideMiddle, true, header_card);
  title_label->SetFullText(file_name_);
  title_label->setStyleSheet("font-size: 14px; font-weight: bold; color: #0F172A; border: none;");
  title_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto *btn_open_file =
      new QPushButton(container_path_.isEmpty() ? Tr("btn_open_file") : Tr("btn_open_parent_archive"), header_card);
  btn_open_file->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px; padding: 4px 10px; border: 1px solid #CBD5E1; border-radius: 4px; background-color: #F8FAFC; "
      "color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }");

  auto *btn_open_folder = new QPushButton(Tr("btn_open_folder"), header_card);
  btn_open_folder->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px; padding: 4px 10px; border: 1px solid #CBD5E1; border-radius: 4px; background-color: #F8FAFC; "
      "color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }");

  title_row->addWidget(icon_label);
  title_row->addWidget(title_label, 1);
  title_row->addWidget(btn_open_file);
  title_row->addWidget(btn_open_folder);
  header_layout->addLayout(title_row);

  path_label_ = new ElidedLabel(Qt::ElideMiddle, true, header_card);
  path_label_->SetFullText(FormatArchiveBreadcrumb(file_path_, container_path_));
  path_label_->setStyleSheet("font-size: 11px; color: #64748B; border: none; font-family: monospace;");
  path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
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

  DocumentPreviewResult preview = DocumentViewerWidget::LoadPreviewDocument(
      doc_id_, file_path_, search_coordinator_, /*allow_disk_fallback=*/container_path_.isEmpty());

  if (!preview.has_content || preview.content.empty()) {
    viewer_->SetHtml(DocumentViewerWidget::BuildStatusNoticeHtml(preview));
    return;
  }

  viewer_->LoadContent(DocumentViewerWidget::FormatPreviewText(preview), keywords_, file_path_);
}

void ContentPreviewDialog::OnOpenFolder() {
  const QString target = container_path_.isEmpty() ? file_path_ : container_path_;
  if (target.isEmpty()) return;
  QFileInfo fi(target);
  QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

void ContentPreviewDialog::OnOpenWithDefaultApp() {
  const QString target = container_path_.isEmpty() ? file_path_ : container_path_;
  if (target.isEmpty()) return;
  QDesktopServices::openUrl(QUrl::fromLocalFile(target));
}

}  // namespace gui
}  // namespace soeasy
