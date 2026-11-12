// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/document_viewer_widget.h"

#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

#include "gui/i18n.h"

namespace soeasy {
namespace gui {

DocumentViewerWidget::DocumentViewerWidget(QWidget *parent) : QWidget(parent) {
  SetupUi();
  Clear();
}

void DocumentViewerWidget::SetupUi() {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  // 1. Navigation / Keyword bar
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
  layout->addLayout(nav_layout);

  // 2. Document Content Viewer
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
  layout->addWidget(text_viewer_, 1);

  connect(btn_next_, &QPushButton::clicked, this, &DocumentViewerWidget::FindNext);
  connect(btn_prev_, &QPushButton::clicked, this, &DocumentViewerWidget::FindPrev);

  RetranslateUi();
}

void DocumentViewerWidget::RetranslateUi() {
  if (label_jump_) label_jump_->setText(Tr("keyword_label"));
  if (btn_prev_) btn_prev_->setText(Tr("btn_prev"));
  if (btn_next_) btn_next_->setText(Tr("btn_next"));

  if (current_keywords_.empty()) {
    if (keyword_tag_label_) keyword_tag_label_->setText(Tr("no_keywords"));
  }
  if (match_count_label_) {
    if (match_ranges_.empty()) {
      match_count_label_->setText(Tr("match_count").arg(0).arg(0));
    } else {
      int cur = (current_match_idx_ >= 0) ? current_match_idx_ + 1 : 1;
      match_count_label_->setText(Tr("match_count").arg(cur).arg(match_ranges_.size()));
    }
  }
}

void DocumentViewerWidget::Clear() {
  current_keywords_.clear();
  match_ranges_.clear();
  current_match_idx_ = -1;

  if (text_viewer_) {
    text_viewer_->clear();
    text_viewer_->setExtraSelections({});
  }

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

  if (btn_prev_) btn_prev_->setEnabled(false);
  if (btn_next_) btn_next_->setEnabled(false);
  if (match_count_label_) match_count_label_->setText(Tr("match_count").arg(0).arg(0));
}

void DocumentViewerWidget::SetHtml(const QString &html) {
  Clear();
  if (text_viewer_) {
    text_viewer_->setHtml(html);
  }
}

void DocumentViewerWidget::LoadContent(const QString &text, const std::vector<std::string> &keywords) {
  current_keywords_ = keywords;
  match_ranges_.clear();
  current_match_idx_ = -1;

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

  text_viewer_->setPlainText(text);
  HighlightOccurrences();
}

void DocumentViewerWidget::HighlightOccurrences() {
  match_ranges_.clear();
  current_match_idx_ = -1;

  if (current_keywords_.empty() || text_viewer_->toPlainText().isEmpty()) {
    text_viewer_->setExtraSelections({});
    match_count_label_->setText(Tr("match_count").arg(0).arg(0));
    btn_prev_->setEnabled(false);
    btn_next_->setEnabled(false);
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

void DocumentViewerWidget::UpdateHighlightSelections() {
  QList<QTextEdit::ExtraSelection> selections;
  QTextDocument *doc = text_viewer_->document();
  if (!doc || match_ranges_.empty()) {
    text_viewer_->setExtraSelections(selections);
    return;
  }

  // 1. 普通匹配项样式：浅黄色背景 + 深琥珀色文字
  QTextCharFormat normal_fmt;
  normal_fmt.setBackground(QColor("#FEF08A"));
  normal_fmt.setForeground(QColor("#854D0E"));
  normal_fmt.setFontWeight(QFont::DemiBold);

  // 2. 当前激活焦点项样式：高对比橙色背景 + 纯白粗体文字
  QTextCharFormat active_fmt;
  active_fmt.setBackground(QColor("#EA580C"));
  active_fmt.setForeground(QColor("#FFFFFF"));
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

void DocumentViewerWidget::JumpToMatch(int index) {
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

  const auto &range = match_ranges_[current_match_idx_];
  QTextCursor cursor(text_viewer_->document());
  cursor.setPosition(range.start);
  cursor.setPosition(range.end, QTextCursor::KeepAnchor);
  text_viewer_->setTextCursor(cursor);
  text_viewer_->ensureCursorVisible();
}

void DocumentViewerWidget::FindNext() {
  if (match_ranges_.empty()) return;
  int next_idx = (current_match_idx_ + 1) % match_ranges_.size();
  JumpToMatch(next_idx);
}

void DocumentViewerWidget::FindPrev() {
  if (match_ranges_.empty()) return;
  int prev_idx = (current_match_idx_ - 1 + static_cast<int>(match_ranges_.size())) % match_ranges_.size();
  JumpToMatch(prev_idx);
}

bool DocumentViewerWidget::ReadDocumentContent(int64_t doc_id, const QString &file_path,
                                               std::shared_ptr<storage::StorageEngine> storage,
                                               std::string *out_content) {
  if (!out_content) return false;
  out_content->clear();

  bool ok = false;
  if (storage && doc_id > 0) {
    ok = storage->GetDocumentContent(doc_id, out_content);
  }

  // Fallback: direct disk read capped at 1MB to prevent memory/UI hangs
  if (!ok || out_content->empty()) {
    constexpr uint64_t kMaxDirectDiskReadBytes = 1024 * 1024;
    QFileInfo fi(file_path);
    if (fi.size() > 0) {
      QFile f(file_path);
      if (f.open(QIODevice::ReadOnly)) {
        QByteArray bytes = f.read(kMaxDirectDiskReadBytes);
        *out_content = std::string(bytes.constData(), bytes.size());
        ok = !out_content->empty();
      }
    }
  }

  return ok && !out_content->empty();
}

}  // namespace gui
}  // namespace soeasy
