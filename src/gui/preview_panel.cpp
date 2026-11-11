#include "gui/preview_panel.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>
#include <chrono>
#include <fstream>

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

PreviewPanel::PreviewPanel(QWidget *parent) : QWidget(parent) {
  SetupUi();
  ClearPreview();
}

void PreviewPanel::SetStorageEngine(std::shared_ptr<storage::StorageEngine> storage) {
  storage_ = std::move(storage);
}

void PreviewPanel::SetupUi() {
  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(6);

  // 1. Header Card: File Info & Quick Actions
  auto *header_card = new QWidget(this);
  header_card->setObjectName("PreviewHeaderCard");
  header_card->setStyleSheet(
      "#PreviewHeaderCard {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "}");
  auto *header_layout = new QVBoxLayout(header_card);
  header_layout->setContentsMargins(10, 8, 10, 8);
  header_layout->setSpacing(4);

  auto *title_row = new QHBoxLayout();
  title_row->setSpacing(8);

  icon_label_ = new QLabel(header_card);
  icon_label_->setFixedSize(24, 24);
  icon_label_->setAlignment(Qt::AlignCenter);
  icon_label_->setStyleSheet("border: none; background: transparent; padding: 0px;");

  title_label_ = new QLabel("未选中文件", header_card);
  title_label_->setStyleSheet("font-size: 13px; font-weight: bold; color: #1E293B; border: none;");

  btn_open_file_ = new QPushButton("打开文件", header_card);
  btn_open_file_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 4px 10px;"
      "  border: 1px solid #BAE6FD;"
      "  border-radius: 4px;"
      "  background-color: #E0F2FE;"
      "  color: #0369A1;"
      "}"
      "QPushButton:hover { background-color: #BAE6FD; color: #0284C7; }"
      "QPushButton:pressed { background-color: #7DD3FC; }");

  btn_open_folder_ = new QPushButton("所在目录", header_card);
  btn_open_folder_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 4px 10px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #F8FAFC;"
      "  color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }");

  title_row->addWidget(icon_label_);
  title_row->addWidget(title_label_, 1);
  title_row->addWidget(btn_open_file_);
  title_row->addWidget(btn_open_folder_);
  header_layout->addLayout(title_row);

  path_label_ = new QLabel(header_card);
  path_label_->setStyleSheet("font-size: 11px; color: #64748B; border: none; font-family: monospace;");
  path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  path_label_->setWordWrap(true);
  header_layout->addWidget(path_label_);

  main_layout->addWidget(header_card);

  // 2. Navigation / In-Document Search Bar
  auto *nav_layout = new QHBoxLayout();
  nav_layout->setSpacing(6);

  label_jump_ = new QLabel("关键词:", this);
  label_jump_->setStyleSheet("font-size: 12px; color: #475569; font-weight: 500;");

  keyword_tag_label_ = new QLabel(this);
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
  keyword_tag_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  btn_prev_ = new QPushButton("▲ 上一个", this);
  btn_prev_->setEnabled(false);
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
  btn_next_ = new QPushButton("▼ 下一个", this);
  btn_next_->setEnabled(false);
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

  match_count_label_ = new QLabel("匹配: 0 / 0", this);
  match_count_label_->setStyleSheet("font-size: 11px; color: #64748B;");

  nav_layout->addWidget(label_jump_);
  nav_layout->addWidget(keyword_tag_label_);
  nav_layout->addStretch();
  nav_layout->addWidget(btn_prev_);
  nav_layout->addWidget(btn_next_);
  nav_layout->addWidget(match_count_label_);
  main_layout->addLayout(nav_layout);

  // 3. Document Content Viewer
  text_viewer_ = new QTextBrowser(this);
  text_viewer_->setReadOnly(true);
  text_viewer_->setStyleSheet(
      "QTextBrowser {"
      "  font-family: 'Consolas', 'Courier New', 'Noto Sans CJK SC', monospace;"
      "  font-size: 12px;"
      "  line-height: 1.5;"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: 8px;"
      "  color: #1E293B;"
      "}"
      "QScrollBar:vertical {"
      "  width: 6px;"
      "  background: transparent;"
      "  margin: 0px;"
      "  padding: 0px;"
      "}"
      "QScrollBar::handle:vertical {"
      "  background: #CBD5E1;"
      "  min-height: 24px;"
      "  border-radius: 3px;"
      "}"
      "QScrollBar::handle:vertical:hover {"
      "  background: #94A3B8;"
      "}"
      "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
      "  height: 0px;"
      "  background: transparent;"
      "}"
      "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
      "  background: transparent;"
      "}");
  main_layout->addWidget(text_viewer_, 1);

  // 4. Meta Status Label
  meta_label_ = new QLabel(this);
  meta_label_->setStyleSheet("font-size: 11px; color: #64748B; padding: 0 4px;");
  main_layout->addWidget(meta_label_);

  // Connections
  connect(btn_open_file_, &QPushButton::clicked, this, &PreviewPanel::OnOpenWithDefaultApp);
  connect(btn_open_folder_, &QPushButton::clicked, this, &PreviewPanel::OnOpenFolder);
  connect(btn_next_, &QPushButton::clicked, this, &PreviewPanel::OnFindNext);
  connect(btn_prev_, &QPushButton::clicked, this, &PreviewPanel::OnFindPrev);

  RetranslateUi();
}

void PreviewPanel::RetranslateUi() {
  if (btn_open_file_) {
    btn_open_file_->setText(Tr("btn_open_file"));
  }
  if (btn_open_folder_) {
    btn_open_folder_->setText(Tr("btn_open_folder"));
  }
  if (label_jump_) {
    label_jump_->setText(Tr("keyword_label"));
  }
  if (btn_prev_) {
    btn_prev_->setText(Tr("btn_prev"));
  }
  if (btn_next_) {
    btn_next_->setText(Tr("btn_next"));
  }
  if (current_keywords_.empty()) {
    if (keyword_tag_label_) {
      keyword_tag_label_->setText(Tr("no_keywords"));
    }
  }
  if (match_count_label_) {
    if (match_ranges_.empty()) {
      match_count_label_->setText(Tr("match_count").arg(0).arg(0));
    } else {
      int cur = (current_match_idx_ >= 0) ? current_match_idx_ + 1 : 1;
      match_count_label_->setText(Tr("match_count").arg(cur).arg(match_ranges_.size()));
    }
  }
  if (current_file_path_.isEmpty()) {
    if (title_label_) title_label_->setText(Tr("no_file_selected"));
    if (path_label_) path_label_->setText(Tr("no_content_extracted"));
  }
}

void PreviewPanel::ClearPreview() {
  current_doc_id_ = 0;
  current_file_name_.clear();
  current_file_path_.clear();
  current_keywords_.clear();
  match_ranges_.clear();
  current_match_idx_ = -1;
  if (text_viewer_) {
    text_viewer_->setExtraSelections({});
  }

  icon_label_->clear();
  title_label_->setText(Tr("no_file_selected"));
  path_label_->setText(Tr("no_content_extracted"));
  btn_open_file_->setEnabled(false);
  btn_open_folder_->setEnabled(false);
  if (keyword_tag_label_) {
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
  btn_prev_->setEnabled(false);
  btn_next_->setEnabled(false);
  match_count_label_->setText(Tr("match_count").arg(0).arg(0));

  text_viewer_->clear();
  meta_label_->clear();
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

  if (!keywords.empty()) {
    QString joined_kw;
    for (size_t i = 0; i < keywords.size(); ++i) {
      if (i > 0) joined_kw += ", ";
      joined_kw += QString::fromStdString(keywords[i]);
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

  std::string decompressed_utf8;
  bool ok = false;
  double decompress_ms = 0.0;

  if (storage_ && doc_id > 0) {
    auto t0 = std::chrono::high_resolution_clock::now();
    ok = storage_->GetDocumentContent(doc_id, &decompressed_utf8);
    auto t1 = std::chrono::high_resolution_clock::now();
    decompress_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  }

  // Fallback: read directly from local file on disk (capped at 1MB to prevent UI hang)
  if (!ok || decompressed_utf8.empty()) {
    constexpr uint64_t kMaxDirectDiskReadBytes = 1024 * 1024;  // 1 MB
    if (fi.size() > 0) {
      std::ifstream ifs(file_path.toStdString(), std::ios::binary);
      if (ifs.is_open()) {
        uint64_t bytes_to_read = std::min<uint64_t>(fi.size(), kMaxDirectDiskReadBytes);
        decompressed_utf8.resize(bytes_to_read);
        ifs.read(&decompressed_utf8[0], bytes_to_read);
        if (fi.size() > static_cast<qint64>(kMaxDirectDiskReadBytes)) {
          decompressed_utf8.append("\n\n[... 文件较大，已截断展示前 1MB 内容 ...]");
        }
      }
    }
  }

  if (decompressed_utf8.empty()) {
    text_viewer_->setHtml("<p style='color:#94A3B8; font-style:italic;'>无法读取文档内容或正文为空。</p>");
    meta_label_->setText(QString("大小: %1  |  修改时间: %2")
                             .arg(FormatFileSize(fi.size()))
                             .arg(fi.lastModified().toString("yyyy-MM-dd hh:mm")));
    return;
  }

  // Cap maximum displayed characters to protect Qt rich-text layout engine from hanging
  constexpr size_t kMaxDisplayChars = 2 * 1024 * 1024;  // 2 MB
  if (decompressed_utf8.size() > kMaxDisplayChars) {
    decompressed_utf8.resize(kMaxDisplayChars);
    decompressed_utf8.append("\n\n[... 内容过长，已截断展示 ...]");
  }

  text_viewer_->setPlainText(QString::fromStdString(decompressed_utf8));
  HighlightOccurrences();

  QString meta_info = QString("大小: %1  |  修改时间: %2")
                          .arg(FormatFileSize(fi.size()))
                          .arg(fi.lastModified().toString("yyyy-MM-dd hh:mm"));
  if (decompress_ms > 0.0) {
    meta_info += QString("  |  解密耗时: %1 ms").arg(decompress_ms, 0, 'f', 2);
  }
  meta_label_->setText(meta_info);
}

void PreviewPanel::HighlightOccurrences() {
  match_ranges_.clear();
  current_match_idx_ = -1;

  if (current_keywords_.empty() || text_viewer_->toPlainText().isEmpty()) {
    text_viewer_->setExtraSelections({});
    match_count_label_->setText("匹配: 0 / 0");
    return;
  }

  QTextDocument *doc = text_viewer_->document();
  const size_t kMaxHighlights = 100;

  for (const auto &kw : current_keywords_) {
    if (kw.empty()) continue;
    QString query_qstr = QString::fromStdString(kw);

    QTextCursor cursor(doc);
    while (true) {
      cursor = doc->find(query_qstr, cursor);
      if (cursor.isNull()) break;

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
    match_count_label_->setText(Tr("match_count")
                                    .arg(1)
                                    .arg(QString("%1%2")
                                             .arg(match_ranges_.size())
                                             .arg(match_ranges_.size() >= kMaxHighlights ? "+" : "")));
    JumpToMatch(0);
  } else {
    text_viewer_->setExtraSelections({});
    btn_prev_->setEnabled(false);
    btn_next_->setEnabled(false);
    match_count_label_->setText(Tr("match_count").arg(0).arg(0));
  }
}

void PreviewPanel::UpdateHighlightSelections() {
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

  // 2. 当前激活焦点项样式：鲜艳高对比橙红色背景 + 纯白粗体文字 (如同 Chrome / VS Code 搜索聚焦)
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

void PreviewPanel::JumpToMatch(int index) {
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

void PreviewPanel::OnFindNext() {
  if (match_ranges_.empty()) return;
  int next_idx = (current_match_idx_ + 1) % match_ranges_.size();
  JumpToMatch(next_idx);
}

void PreviewPanel::OnFindPrev() {
  if (match_ranges_.empty()) return;
  int prev_idx = (current_match_idx_ - 1 + match_ranges_.size()) % match_ranges_.size();
  JumpToMatch(prev_idx);
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

}  // namespace gui
}  // namespace soeasy
