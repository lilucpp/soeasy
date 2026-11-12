// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/preview_panel.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMetaObject>
#include <QThreadPool>
#include <QUrl>
#include <QVBoxLayout>

#include "gui/gui_utils.h"
#include "gui/i18n.h"
#include "gui/icon_provider.h"

namespace soeasy {
namespace gui {

PreviewPanel::PreviewPanel(QWidget *parent) : QWidget(parent) {
  SetupUi();
  ClearPreview();
}

void PreviewPanel::SetStorageEngine(std::shared_ptr<storage::StorageEngine> storage) {
  storage_ = std::move(storage);
}

void PreviewPanel::SetupUi() {
  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(12, 12, 12, 12);
  main_layout->setSpacing(8);

  // 1. Top Card: Document Info & Direct Action Toolbar
  auto *header_card = new QWidget(this);
  header_card->setStyleSheet(
      "background-color: #FFFFFF; border: 1px solid #E2E8F0; border-radius: 6px; padding: 8px;");
  auto *header_layout = new QVBoxLayout(header_card);
  header_layout->setContentsMargins(8, 8, 8, 8);
  header_layout->setSpacing(4);

  auto *title_row = new QHBoxLayout();
  icon_label_ = new QLabel(header_card);
  icon_label_->setStyleSheet("border: none;");

  title_label_ = new QLabel(header_card);
  title_label_->setStyleSheet("font-size: 13px; font-weight: bold; color: #0F172A; border: none;");
  title_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  btn_open_file_ = new QPushButton(header_card);
  btn_open_file_->setEnabled(false);
  btn_open_file_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 3px 8px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #F8FAFC;"
      "  color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }");

  btn_open_folder_ = new QPushButton(header_card);
  btn_open_folder_->setEnabled(false);
  btn_open_folder_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 3px 8px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #F8FAFC;"
      "  color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }");

  btn_close_ = new QPushButton("✕", header_card);
  btn_close_->setCursor(Qt::PointingHandCursor);
  btn_close_->setToolTip("收起预览面板 (Ctrl+P)");
  btn_close_->setStyleSheet(
      "QPushButton {"
      "  font-size: 12px;"
      "  padding: 4px 8px;"
      "  border: 1px solid transparent;"
      "  border-radius: 4px;"
      "  background-color: transparent;"
      "  color: #94A3B8;"
      "}"
      "QPushButton:hover {"
      "  background-color: #F1F5F9;"
      "  color: #475569;"
      "  border-color: #E2E8F0;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #E2E8F0;"
      "}");

  title_row->addWidget(icon_label_);
  title_row->addWidget(title_label_, 1);
  title_row->addWidget(btn_open_file_);
  title_row->addWidget(btn_open_folder_);
  title_row->addWidget(btn_close_);
  header_layout->addLayout(title_row);

  path_label_ = new QLabel(header_card);
  path_label_->setStyleSheet("font-size: 11px; color: #64748B; border: none; font-family: monospace;");
  path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  path_label_->setWordWrap(true);
  header_layout->addWidget(path_label_);

  main_layout->addWidget(header_card);

  // 2. Reusable Document Viewer Widget
  viewer_ = new DocumentViewerWidget(this);
  main_layout->addWidget(viewer_, 1);

  // Connections
  connect(btn_open_file_, &QPushButton::clicked, this, &PreviewPanel::OnOpenWithDefaultApp);
  connect(btn_open_folder_, &QPushButton::clicked, this, &PreviewPanel::OnOpenFolder);
  connect(btn_close_, &QPushButton::clicked, this, &PreviewPanel::CloseRequested);

  RetranslateUi();
}

void PreviewPanel::RetranslateUi() {
  if (btn_open_file_) {
    btn_open_file_->setText(Tr("btn_open_file"));
  }
  if (btn_open_folder_) {
    btn_open_folder_->setText(Tr("btn_open_folder"));
  }
  if (btn_close_) {
    btn_close_->setToolTip(Tr("tip_close_preview"));
  }
  if (viewer_) {
    viewer_->RetranslateUi();
  }
  if (current_file_path_.isEmpty()) {
    if (title_label_) title_label_->setText(Tr("no_file_selected"));
    if (path_label_) path_label_->setText(Tr("no_content_extracted"));
    RenderEmptyStateGuide();
  }
}

void PreviewPanel::WaitForLoadFinished(int timeout_ms) {
  QThreadPool::globalInstance()->waitForDone(timeout_ms);
}

void PreviewPanel::ClearPreview() {
  ++load_request_id_;
  current_doc_id_ = 0;
  current_file_name_.clear();
  current_file_path_.clear();
  current_keywords_.clear();

  if (icon_label_) icon_label_->clear();
  if (title_label_) title_label_->setText(Tr("no_file_selected"));
  if (path_label_) path_label_->setText(Tr("no_content_extracted"));
  if (btn_open_file_) btn_open_file_->setEnabled(false);
  if (btn_open_folder_) btn_open_folder_->setEnabled(false);

  RenderEmptyStateGuide();
}

void PreviewPanel::LoadDocument(int64_t doc_id, const QString &file_name, const QString &file_path,
                                const std::vector<std::string> &keywords) {
  current_doc_id_ = doc_id;
  current_file_name_ = file_name;
  current_file_path_ = file_path;
  current_keywords_ = keywords;

  QFileInfo fi(file_path);
  QString ext = fi.suffix();

  icon_label_->setPixmap(IconProvider::GetFileIcon(ext).pixmap(24, 24));
  title_label_->setText(file_name);
  path_label_->setText(file_path);

  btn_open_file_->setEnabled(true);
  btn_open_folder_->setEnabled(true);

  uint64_t req_id = ++load_request_id_;
  viewer_->SetHtml(QString("<p style='color:#94A3B8; font-style:italic;'>%1</p>").arg(Tr("preview_loading_content")));

  auto storage = storage_;
  QThreadPool::globalInstance()->start([this, req_id, doc_id, file_path, keywords, storage]() {
    std::string decompressed_utf8;
    bool ok = DocumentViewerWidget::ReadDocumentContent(doc_id, file_path, storage, &decompressed_utf8);

    QMetaObject::invokeMethod(
        this,
        [this, req_id, ok, content = std::move(decompressed_utf8), keywords]() {
          if (req_id != load_request_id_) {
            return;
          }
          if (!ok || content.empty()) {
            viewer_->SetHtml(QString("<p style='color:#94A3B8; font-style:italic;'>%1</p>").arg(Tr("preview_cannot_read")));
            return;
          }
          viewer_->LoadContent(QString::fromStdString(content), keywords);
        },
        Qt::QueuedConnection);
  });
}

void PreviewPanel::OnOpenFolder() {
  if (current_file_path_.isEmpty()) return;
  QFileInfo fi(current_file_path_);
  QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

void PreviewPanel::OnOpenWithDefaultApp() {
  if (current_file_path_.isEmpty()) return;
  QDesktopServices::openUrl(QUrl::fromLocalFile(current_file_path_));
}

void PreviewPanel::RenderEmptyStateGuide() {
  if (!viewer_) return;

  bool is_zh = (I18n::Instance().current_language() == Language::kChinese);

  QString title = is_zh ? "SoEasy 桌面搜索引擎" : "SoEasy Desktop Search";
  QString subtitle = is_zh ? "让电脑没有难搜的文件 · 快捷操作速查" : "Fast, local & secure desktop search · Quick Reference";

  QString kbd_title = is_zh ? "⌨️ 常用快捷键" : "⌨️ Keyboard Shortcuts";
  QString tips_title = is_zh ? "💡 高效搜索技巧" : "💡 Search Tips";

  auto kbd = [](const QString &key) { return GenerateKbdBadgeHtml(key); };

  QString shortcuts_html;
  if (is_zh) {
    shortcuts_html = QString(
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%1 %2</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">在结果列表中切换并实时预览</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%3</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">以默认程序直接打开文件</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%4</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">打开文件所在目录</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%5</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">快速聚焦至搜索框</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%6</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">显示 / 隐藏右侧预览面板</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%7</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">打开系统设置</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%8</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">清空搜索词 / 最小化至托盘</td></tr>")
        .arg(kbd("↑")).arg(kbd("↓"))
        .arg(kbd("Enter"))
        .arg(kbd("Ctrl + O"))
        .arg(kbd("Ctrl + F"))
        .arg(kbd("Ctrl + P"))
        .arg(kbd("Ctrl + ,"))
        .arg(kbd("Esc"));
  } else {
    shortcuts_html = QString(
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%1 %2</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">Navigate results with live preview</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%3</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">Open file with default program</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%4</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">Open containing folder</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%5</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">Focus search input box</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%6</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">Toggle right preview panel</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%7</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">Open System Settings</td></tr>"
        "<tr><td width=\"130\" align=\"left\" valign=\"middle\">%8</td><td align=\"left\" valign=\"middle\" style=\"color:#334155; font-size:12px; padding-left:10px;\">Clear query / hide to tray</td></tr>")
        .arg(kbd("↑")).arg(kbd("↓"))
        .arg(kbd("Enter"))
        .arg(kbd("Ctrl + O"))
        .arg(kbd("Ctrl + F"))
        .arg(kbd("Ctrl + P"))
        .arg(kbd("Ctrl + ,"))
        .arg(kbd("Esc"));
  }

  QString tips_html;
  if (is_zh) {
    tips_html =
        "<li><b>全文检索 (FTS5)</b>：多词空格组合（如 <code>报告 2026</code>），毫秒级倒排索引</li>"
        "<li><b>文件名秒搜</b>：点击搜索栏左侧模式下拉可秒级查找任意文件名</li>"
        "<li><b>管理搜索目录</b>：点击右上角【⚙ 系统设置】可自由增减受索引监控的文件夹</li>";
  } else {
    tips_html =
        "<li><b>Full-text Search (FTS5)</b>: Multi-word space combinations (e.g. <code>report 2026</code>)</li>"
        "<li><b>Instant Filename</b>: Toggle mode to instant filename matching across all files</li>"
        "<li><b>Manage Directories</b>: Click [⚙ System Settings] to configure monitored folders</li>";
  }

  QString html = QString(
      "<div style=\"font-family: 'Microsoft YaHei UI', 'Segoe UI', 'Noto Sans CJK SC', sans-serif; "
      "color:#1E293B; padding:16px 14px; line-height:1.6;\">"
      "  <div style=\"text-align:center; margin-bottom:18px;\">"
      "    <div style=\"font-size:16px; font-weight:bold; color:#0284C7; letter-spacing:0.5px;\">%1</div>"
      "    <div style=\"font-size:12px; color:#64748B; margin-top:3px;\">%2</div>"
      "  </div>"
      "  <table width=\"100%\" border=\"0\" cellpadding=\"5\" cellspacing=\"0\" style=\"background-color:#F8FAFC; border:1px solid #E2E8F0; border-radius:8px; margin-bottom:14px;\">"
      "    <tr><td colspan=\"2\" style=\"font-size:12px; font-weight:bold; color:#334155; padding:8px 8px 4px 8px;\">%3</td></tr>"
      "    %4"
      "  </table>"
      "  <div style=\"background-color:#F0FDF4; border:1px solid #DCFCE7; border-radius:8px; padding:12px 14px;\">"
      "    <div style=\"font-size:12px; font-weight:bold; color:#15803D; margin-bottom:6px;\">%5</div>"
      "    <ul style=\"margin:0; padding-left:18px; font-size:12px; color:#166534; line-height:1.7;\">%6</ul>"
      "  </div>"
      "</div>")
      .arg(title)
      .arg(subtitle)
      .arg(kbd_title)
      .arg(shortcuts_html)
      .arg(tips_title)
      .arg(tips_html);

  viewer_->SetHtml(html);
}

}  // namespace gui
}  // namespace soeasy
