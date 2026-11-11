#include "gui/content_preview_dialog.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include "gui/i18n.h"

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
  setWindowTitle(Tr("preview_dlg_title").arg(file_name_));
  resize(900, 680);
  SetupUi();
  LoadContentAndHighlight();
}

void ContentPreviewDialog::SetupUi() {
  setStyleSheet(
      "QDialog {"
      "  background-color: #FFFFFF;"
      "  color: #1E293B;"
      "  font-family: 'Microsoft YaHei UI', 'Microsoft YaHei', 'Segoe UI', 'PingFang SC', 'Noto Sans CJK SC', "
      "'WenQuanYi Micro Hei', sans-serif;"
      "}");

  auto *main_layout = new QVBoxLayout(this);
  main_layout->setSpacing(8);

  // Top header row: File path and actions
  auto *top_layout = new QHBoxLayout();
  path_label_ = new QLabel(file_path_, this);
  path_label_->setStyleSheet("font-size: 12px; color: #546E7A; font-family: monospace;");
  path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto *btn_open_folder = new QPushButton(Tr("btn_open_folder_sys"), this);
  auto *btn_open_app = new QPushButton(Tr("btn_open_app_sys"), this);
  btn_open_folder->setStyleSheet("padding: 4px 10px;");
  btn_open_app->setStyleSheet("padding: 4px 10px;");

  top_layout->addWidget(path_label_, 1);
  top_layout->addWidget(btn_open_folder);
  top_layout->addWidget(btn_open_app);
  main_layout->addLayout(top_layout);

  connect(btn_open_folder, &QPushButton::clicked, this, &ContentPreviewDialog::OnOpenFolder);
  connect(btn_open_app, &QPushButton::clicked, this, &ContentPreviewDialog::OnOpenWithDefaultApp);

  // Search/Navigation bar inside document
  auto *nav_layout = new QHBoxLayout();
  auto *label_search = new QLabel(Tr("keyword_label"), this);
  label_search->setStyleSheet("font-size: 12px; color: #475569; font-weight: 500;");

  keyword_tag_label_ = new QLabel(this);
  if (!keywords_.empty()) {
    QString joined_kw;
    for (size_t i = 0; i < keywords_.size(); ++i) {
      if (i > 0) joined_kw += ", ";
      joined_kw += QString::fromStdString(keywords_[i]);
    }
    keyword_tag_label_->setText(joined_kw);
    keyword_tag_label_->setStyleSheet(
        "QLabel {"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "  color: #B45309;"
        "  background-color: #FEF3C7;"
        "  border: 1px solid #FDE68A;"
        "  border-radius: 4px;"
        "  padding: 3px 8px;"
        "}");
  } else {
    keyword_tag_label_->setText(Tr("no_keywords"));
    keyword_tag_label_->setStyleSheet(
        "QLabel {"
        "  font-size: 12px;"
        "  font-weight: normal;"
        "  color: #64748B;"
        "  background-color: #F1F5F9;"
        "  border: 1px solid #E2E8F0;"
        "  border-radius: 4px;"
        "  padding: 3px 8px;"
        "}");
  }
  keyword_tag_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  btn_prev_ = new QPushButton(Tr("btn_prev"), this);
  btn_next_ = new QPushButton(Tr("btn_next"), this);
  btn_prev_->setEnabled(false);
  btn_next_->setEnabled(false);
  btn_prev_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 3px 8px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #F8FAFC;"
      "  color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }"
      "QPushButton:disabled { color: #94A3B8; background-color: #F8FAFC; border-color: #E2E8F0; }");
  btn_next_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 3px 8px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #F8FAFC;"
      "  color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }"
      "QPushButton:disabled { color: #94A3B8; background-color: #F8FAFC; border-color: #E2E8F0; }");

  match_count_label_ = new QLabel(Tr("match_count").arg(0).arg(0), this);
  match_count_label_->setStyleSheet("font-size: 11px; color: #64748B;");

  nav_layout->addWidget(label_search);
  nav_layout->addWidget(keyword_tag_label_);
  nav_layout->addStretch();
  nav_layout->addWidget(btn_prev_);
  nav_layout->addWidget(btn_next_);
  nav_layout->addWidget(match_count_label_);
  main_layout->addLayout(nav_layout);

  connect(btn_next_, &QPushButton::clicked, this, &ContentPreviewDialog::OnFindNext);
  connect(btn_prev_, &QPushButton::clicked, this, &ContentPreviewDialog::OnFindPrev);

  // Text browser for full document view
  text_viewer_ = new QTextBrowser(this);
  text_viewer_->setReadOnly(true);
  text_viewer_->setStyleSheet(
      "QTextBrowser {"
      "  font-family: 'Consolas', 'Courier New', 'Noto Sans CJK SC', monospace;"
      "  font-size: 13px;"
      "  line-height: 1.5;"
      "  background-color: #FAFAFA;"
      "  border: 1px solid #CFD8DC;"
      "  border-radius: 4px;"
      "  padding: 8px;"
      "}");
  main_layout->addWidget(text_viewer_, 1);
}

void ContentPreviewDialog::LoadContentAndHighlight() {
  std::string decompressed_utf8;
  bool ok = false;
  double decompress_ms = 0.0;

  if (storage_ && doc_id_ > 0) {
    auto t0 = std::chrono::high_resolution_clock::now();
    ok = storage_->GetDocumentContent(doc_id_, &decompressed_utf8);
    auto t1 = std::chrono::high_resolution_clock::now();
    decompress_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  }

  // Fallback: read directly from local file on disk (capped at 2MB to prevent UI hang)
  if (!ok || decompressed_utf8.empty()) {
    QFileInfo fi(file_path_);
    constexpr uint64_t kMaxDirectDiskReadBytes = 2 * 1024 * 1024;  // 2 MB
    if (fi.size() > 0) {
      std::ifstream ifs(file_path_.toStdString(), std::ios::binary);
      if (ifs.is_open()) {
        uint64_t bytes_to_read = std::min<uint64_t>(fi.size(), kMaxDirectDiskReadBytes);
        decompressed_utf8.resize(bytes_to_read);
        ifs.read(&decompressed_utf8[0], bytes_to_read);
        if (fi.size() > static_cast<qint64>(kMaxDirectDiskReadBytes)) {
          decompressed_utf8.append("\n\n[... 文件较大，已截断展示前 2MB 内容 ...]");
        }
      }
    }
  }

  if (decompressed_utf8.empty()) {
    text_viewer_->setHtml("<p style='color:#B0BEC5; font-style:italic;'>无法读取文档内容或文档为空。</p>");
    return;
  }

  // Cap maximum displayed characters to protect Qt rich-text layout engine from hanging
  constexpr size_t kMaxDisplayChars = 2 * 1024 * 1024;  // 2 MB
  if (decompressed_utf8.size() > kMaxDisplayChars) {
    decompressed_utf8.resize(kMaxDisplayChars);
    decompressed_utf8.append("\n\n[... 内容过长，已截断展示 ...]");
  }

  if (decompress_ms > 0.0 && path_label_) {
    path_label_->setToolTip(QString("文件路径: %1\n密态解密耗时: %2 ms").arg(file_path_).arg(decompress_ms, 0, 'f', 2));
  }

  text_viewer_->setPlainText(QString::fromStdString(decompressed_utf8));
  HighlightOccurrences();
}

void ContentPreviewDialog::HighlightOccurrences() {
  match_ranges_.clear();
  current_match_idx_ = -1;

  if (keywords_.empty() || text_viewer_->toPlainText().isEmpty()) {
    text_viewer_->setExtraSelections({});
    match_count_label_->setText("匹配: 0 / 0");
    return;
  }

  QTextDocument *doc = text_viewer_->document();
  const size_t kMaxHighlights = 150;

  for (const auto &kw : keywords_) {
    if (kw.empty()) continue;
    QString query_qstr = QString::fromStdString(kw);

    QTextCursor cursor(doc);
    while (true) {
      cursor = doc->find(query_qstr, cursor, QTextDocument::FindFlags());
      if (cursor.isNull()) {
        break;
      }
      match_ranges_.push_back({cursor.selectionStart(), cursor.selectionEnd()});
      if (match_ranges_.size() >= kMaxHighlights) {
        break;
      }
    }
    if (match_ranges_.size() >= kMaxHighlights) {
      break;
    }
  }

  if (!match_ranges_.empty()) {
    std::sort(match_ranges_.begin(), match_ranges_.end(),
              [](const MatchRange &a, const MatchRange &b) { return a.start < b.start; });
    match_ranges_.erase(
        std::unique(match_ranges_.begin(), match_ranges_.end(),
                    [](const MatchRange &a, const MatchRange &b) { return a.start == b.start && a.end == b.end; }),
        match_ranges_.end());

    btn_prev_->setEnabled(true);
    btn_next_->setEnabled(true);
    JumpToMatch(0);
  } else {
    text_viewer_->setExtraSelections({});
    btn_prev_->setEnabled(false);
    btn_next_->setEnabled(false);
    match_count_label_->setText(Tr("match_count").arg(0).arg(0));
  }
}

void ContentPreviewDialog::UpdateHighlightSelections() {
  QList<QTextEdit::ExtraSelection> selections;
  QTextDocument *doc = text_viewer_->document();
  if (!doc || match_ranges_.empty()) {
    text_viewer_->setExtraSelections(selections);
    return;
  }

  // 1. 普通匹配项样式：柔和浅黄色背景 + 深琥珀色文字
  QTextCharFormat normal_fmt;
  normal_fmt.setBackground(QColor("#FEF08A"));  // 黄色高亮 (amber-200)
  normal_fmt.setForeground(QColor("#854D0E"));  // 深褐/琥珀色 (amber-800)
  normal_fmt.setFontWeight(QFont::DemiBold);

  // 2. 当前激活焦点项样式：鲜艳高对比橙红色背景 + 纯白粗体文字
  QTextCharFormat active_fmt;
  active_fmt.setBackground(QColor("#EA580C"));  // 活力鲜亮橙 (orange-600)
  active_fmt.setForeground(QColor("#FFFFFF"));  // 纯白清晰文字
  active_fmt.setFontWeight(QFont::Bold);

  selections.reserve(static_cast<int>(match_ranges_.size()));

  for (size_t i = 0; i < match_ranges_.size(); ++i) {
    QTextCursor cursor(doc);
    cursor.setPosition(match_ranges_[i].start);
    cursor.setPosition(match_ranges_[i].end, QTextCursor::KeepAnchor);

    QTextEdit::ExtraSelection sel;
    sel.cursor = cursor;
    sel.format = (static_cast<int>(i) == current_match_idx_) ? active_fmt : normal_fmt;
    selections.append(sel);
  }

  text_viewer_->setExtraSelections(selections);
}

void ContentPreviewDialog::JumpToMatch(int index) {
  if (match_ranges_.empty()) {
    match_count_label_->setText(Tr("match_count").arg(0).arg(0));
    return;
  }

  if (index < 0) {
    index = static_cast<int>(match_ranges_.size()) - 1;
  } else if (index >= static_cast<int>(match_ranges_.size())) {
    index = 0;
  }

  current_match_idx_ = index;
  match_count_label_->setText(Tr("match_count").arg(current_match_idx_ + 1).arg(match_ranges_.size()));

  UpdateHighlightSelections();

  // 滚动视口定位到当前激活的高亮项
  const auto &range = match_ranges_[current_match_idx_];
  QTextCursor cursor(text_viewer_->document());
  cursor.setPosition(range.start);
  cursor.setPosition(range.end, QTextCursor::KeepAnchor);
  text_viewer_->setTextCursor(cursor);
  text_viewer_->ensureCursorVisible();
}

void ContentPreviewDialog::OnFindNext() {
  JumpToMatch(current_match_idx_ + 1);
}

void ContentPreviewDialog::OnFindPrev() {
  JumpToMatch(current_match_idx_ - 1);
}

void ContentPreviewDialog::OnOpenFolder() {
  QFileInfo fi(file_path_);
  QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

void ContentPreviewDialog::OnOpenWithDefaultApp() {
  QDesktopServices::openUrl(QUrl::fromLocalFile(file_path_));
}

}  // namespace gui
}  // namespace soeasy
