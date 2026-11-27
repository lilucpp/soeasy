// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/document_viewer_widget.h"

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <unordered_set>

#include "common/format_categories.h"
#include "common/logger.h"
#include "gui/i18n.h"
#include "gui/knuth_plass_viewer.h"
#include "gui/typesetting/preview_typography.h"
#include "gui/typography_popover.h"

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

  keyword_tag_label_ = new ElidedLabel(Qt::ElideRight, false, this);
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

  btn_kp_toggle_ = new QPushButton("KP 精排", this);
  btn_kp_toggle_->setCheckable(true);
  btn_kp_toggle_->setChecked(typesetting::PreviewTypographyStore::Instance().config().kp_enabled);
  btn_kp_toggle_->setEnabled(false);
  btn_kp_toggle_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 3px 8px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #F8FAFC;"
      "  color: #475569;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:checked {"
      "  background-color: #EFF6FF;"
      "  border-color: #93C5FD;"
      "  color: #1D4ED8;"
      "  font-weight: 600;"
      "}"
      "QPushButton:disabled { color: #94A3B8; background-color: #F8FAFC; border-color: #E2E8F0; }");

  btn_typography_ = new QPushButton("⚙ 排版", this);
  btn_typography_->setStyleSheet(
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

  nav_layout->addWidget(label_jump_);
  nav_layout->addWidget(keyword_tag_label_);
  nav_layout->addStretch();
  nav_layout->addWidget(btn_prev_);
  nav_layout->addWidget(btn_next_);
  nav_layout->addWidget(match_count_label_);
  layout->addLayout(nav_layout);

  auto *kp_toolbar_layout = new QHBoxLayout();
  kp_toolbar_layout->setContentsMargins(0, 0, 0, 0);
  kp_toolbar_layout->setSpacing(6);
  kp_toolbar_layout->addWidget(btn_kp_toggle_);
  kp_toolbar_layout->addWidget(btn_typography_);
  kp_toolbar_layout->addStretch();
  layout->addLayout(kp_toolbar_layout);

  // 2. Document Content Viewer Stack (QTextBrowser + KnuthPlassViewer)
  viewer_stack_ = new QStackedWidget(this);

  text_viewer_ = new QTextBrowser(this);
  text_viewer_->setReadOnly(true);
  ApplyBrowserStyle(true);

  kp_viewer_ = new KnuthPlassViewer(this);
  kp_viewer_->SetTypographyConfig(typesetting::PreviewTypographyStore::Instance().config());

  viewer_stack_->addWidget(text_viewer_);
  viewer_stack_->addWidget(kp_viewer_);
  viewer_stack_->setCurrentWidget(text_viewer_);
  layout->addWidget(viewer_stack_, 1);

  connect(btn_next_, &QPushButton::clicked, this, &DocumentViewerWidget::FindNext);
  connect(btn_prev_, &QPushButton::clicked, this, &DocumentViewerWidget::FindPrev);
  connect(btn_kp_toggle_, &QPushButton::clicked, this, [this](bool checked) {
    if (!has_plain_text_ || html_mode_) {
      return;
    }
    if (is_code_document_) {
      ApplyActiveViewerMode(checked);
      return;
    }
    auto cfg = typesetting::PreviewTypographyStore::Instance().config();
    cfg.kp_enabled = checked;
    typesetting::PreviewTypographyStore::Instance().UpdateConfig(cfg);
  });
  connect(btn_typography_, &QPushButton::clicked, this, [this]() {
    if (!typography_popover_) {
      typography_popover_ = new TypographyPopover(this);
    }
    typography_popover_->ShowBelow(btn_typography_);
  });
  connect(&typesetting::PreviewTypographyStore::Instance(),
          &typesetting::PreviewTypographyStore::ConfigChanged, this,
          [this](const typesetting::PreviewTypographyConfig &cfg) {
            if (kp_viewer_) {
              kp_viewer_->SetTypographyConfig(cfg);
            }
            if (!has_plain_text_ || html_mode_) {
              QSignalBlocker blocker(btn_kp_toggle_);
              btn_kp_toggle_->setChecked(cfg.kp_enabled);
              return;
            }
            if (!is_code_document_) {
              if (cfg.kp_enabled != kp_active_for_doc_) {
                ApplyActiveViewerMode(cfg.kp_enabled);
              } else if (kp_active_for_doc_) {
                UpdateMatchCountLabel();
                const int total = kp_viewer_ ? kp_viewer_->MatchCount() : 0;
                if (btn_prev_) btn_prev_->setEnabled(total > 0);
                if (btn_next_) btn_next_->setEnabled(total > 0);
              }
            }
          });

  RetranslateUi();
}

void DocumentViewerWidget::ApplyBrowserStyle(bool code_mode) {
  if (!text_viewer_) return;
  const QString font_family =
      code_mode ? "'Consolas', 'JetBrains Mono', 'Noto Sans Mono CJK SC', 'Courier New', monospace"
                : "'WenYuan Sans SC', 'Noto Sans CJK SC', 'Microsoft YaHei', sans-serif";
  const QString font_size = code_mode ? "12px" : "13px";
  const QString padding = code_mode ? "10px 12px" : "14px 18px";
  text_viewer_->setStyleSheet(QString(
      "QTextBrowser {"
      "  font-family: %1;"
      "  font-size: %2;"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: %3;"
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
      "}").arg(font_family, font_size, padding));
}

void DocumentViewerWidget::ApplyBrowserBlockSpacing(double line_height_pct) {
  if (!text_viewer_ || !text_viewer_->document()) return;
  QTextCursor cursor(text_viewer_->document());
  cursor.select(QTextCursor::Document);
  QTextBlockFormat fmt;
  fmt.setLineHeight(line_height_pct, QTextBlockFormat::ProportionalHeight);
  cursor.mergeBlockFormat(fmt);
}

bool DocumentViewerWidget::IsCodeOrStructuredFile(const QString &file_path) {
  if (file_path.isEmpty()) return false;
  const QString suffix = QFileInfo(file_path).suffix().toLower();
  if (suffix.isEmpty()) return false;
  static const std::unordered_set<std::string> kCodeExts = {
      "c",     "cpp",  "cc",   "cxx",  "h",    "hpp",  "hxx",  "py",    "java",
      "go",    "rs",   "js",   "ts",   "jsx",  "tsx",  "sh",   "bat",   "cmd",
      "ps1",   "sql",  "php",  "rb",   "swift","kt",   "scala","lua",   "r",
      "pl",    "perl", "asm",  "asp",  "aspx", "bas",  "cmake","cs",    "d",
      "f",     "fpp",  "fs",   "jsp",  "pas",  "tcl",  "vb",   "vbs",   "ws",
      "css",   "xml",  "xsd",  "xsl",  "csv",  "json", "yml",  "yaml",  "toml",
      "ini",   "conf", "cfg",  "rss",  "log"};
  return kCodeExts.find(suffix.toStdString()) != kCodeExts.end();
}

void DocumentViewerWidget::RetranslateUi() {
  if (label_jump_) label_jump_->setText(Tr("keyword_label"));
  if (btn_prev_) btn_prev_->setText(Tr("btn_prev"));
  if (btn_next_) btn_next_->setText(Tr("btn_next"));
  if (btn_kp_toggle_) {
    btn_kp_toggle_->setText(Tr("btn_kp_toggle"));
    btn_kp_toggle_->setToolTip(Tr("tip_kp_toggle"));
  }
  if (btn_typography_) {
    btn_typography_->setText(Tr("btn_kp_settings"));
    btn_typography_->setToolTip(Tr("tip_kp_settings"));
  }
  if (typography_popover_) {
    typography_popover_->RetranslateUi();
  }

  if (current_keywords_.empty()) {
    if (keyword_tag_label_) keyword_tag_label_->SetFullText(Tr("no_keywords"), false);
  }
  UpdateMatchCountLabel();
}

int DocumentViewerWidget::match_count() const {
  if (kp_active_for_doc_ && kp_viewer_) {
    return kp_viewer_->MatchCount();
  }
  return static_cast<int>(match_ranges_.size());
}

void DocumentViewerWidget::UpdateMatchCountLabel() {
  if (!match_count_label_) {
    return;
  }
  if (kp_active_for_doc_ && kp_viewer_) {
    const int count = kp_viewer_->MatchCount();
    if (count <= 0) {
      match_count_label_->setText(Tr("match_count").arg(0).arg(0));
      return;
    }
    const int cur = (kp_viewer_->CurrentMatchIndex() >= 0) ? kp_viewer_->CurrentMatchIndex() + 1 : 1;
    const QString total_text =
        QString("%1%2").arg(count).arg(kp_viewer_->HighlightsTruncated() ? "+" : "");
    match_count_label_->setText(Tr("match_count").arg(cur).arg(total_text));
    return;
  }
  if (match_ranges_.empty()) {
    match_count_label_->setText(Tr("match_count").arg(0).arg(0));
    return;
  }
  const int cur = (current_match_idx_ >= 0) ? current_match_idx_ + 1 : 1;
  const QString total_text = QString("%1%2").arg(match_ranges_.size()).arg(highlights_truncated_ ? "+" : "");
  match_count_label_->setText(Tr("match_count").arg(cur).arg(total_text));
}

void DocumentViewerWidget::UpdateKeywordBadge(const std::vector<std::string> &keywords) {
  if (!keyword_tag_label_) {
    return;
  }
  if (!keywords.empty()) {
    QString joined_kw;
    for (size_t i = 0; i < keywords.size(); ++i) {
      if (i > 0) joined_kw += ", ";
      joined_kw += QString::fromStdString(keywords[i]);
    }
    keyword_tag_label_->SetFullText(joined_kw);
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
    return;
  }
  keyword_tag_label_->SetFullText(Tr("no_keywords"), false);
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

void DocumentViewerWidget::Clear() {
  current_keywords_.clear();
  match_ranges_.clear();
  current_match_idx_ = -1;
  highlights_truncated_ = false;
  current_plain_text_.clear();
  current_file_path_.clear();
  has_plain_text_ = false;
  html_mode_ = false;
  is_code_document_ = false;
  kp_active_for_doc_ = false;

  if (kp_viewer_) {
    kp_viewer_->Clear();
  }
  if (text_viewer_) {
    ApplyBrowserStyle(true);
    text_viewer_->clear();
    text_viewer_->setExtraSelections({});
  }
  if (viewer_stack_ && text_viewer_) {
    viewer_stack_->setCurrentWidget(text_viewer_);
  }

  if (btn_kp_toggle_) {
    QSignalBlocker blocker(btn_kp_toggle_);
    btn_kp_toggle_->setEnabled(false);
    btn_kp_toggle_->setChecked(typesetting::PreviewTypographyStore::Instance().config().kp_enabled);
  }

  UpdateKeywordBadge({});

  if (btn_prev_) btn_prev_->setEnabled(false);
  if (btn_next_) btn_next_->setEnabled(false);
  if (match_count_label_) match_count_label_->setText(Tr("match_count").arg(0).arg(0));
}

void DocumentViewerWidget::SetHtml(const QString &html) {
  Clear();
  html_mode_ = true;
  if (text_viewer_) {
    text_viewer_->setHtml(html);
  }
}

void DocumentViewerWidget::ApplyActiveViewerMode(bool use_kp) {
  kp_active_for_doc_ = use_kp;
  if (btn_kp_toggle_) {
    QSignalBlocker blocker(btn_kp_toggle_);
    btn_kp_toggle_->setEnabled(has_plain_text_);
    btn_kp_toggle_->setChecked(use_kp);
  }

  if (use_kp && kp_viewer_) {
    match_ranges_.clear();
    current_match_idx_ = -1;
    highlights_truncated_ = false;
    if (text_viewer_) {
      text_viewer_->clear();
      text_viewer_->setExtraSelections({});
    }
    kp_viewer_->SetTypographyConfig(typesetting::PreviewTypographyStore::Instance().config());
    kp_viewer_->SetContent(current_plain_text_, current_keywords_);
    if (viewer_stack_) viewer_stack_->setCurrentWidget(kp_viewer_);

    const bool has_matches = kp_viewer_->MatchCount() > 0;
    if (btn_prev_) btn_prev_->setEnabled(has_matches);
    if (btn_next_) btn_next_->setEnabled(has_matches);
    UpdateMatchCountLabel();
    return;
  }

  if (kp_viewer_) {
    kp_viewer_->Clear();
  }
  ApplyBrowserStyle(is_code_document_);
  if (text_viewer_) {
    text_viewer_->setPlainText(current_plain_text_);
    ApplyBrowserBlockSpacing(is_code_document_ ? 145.0 : 160.0);
  }
  if (viewer_stack_ && text_viewer_) {
    viewer_stack_->setCurrentWidget(text_viewer_);
  }
  HighlightOccurrences();
}

void DocumentViewerWidget::LoadContent(const QString &text, const std::vector<std::string> &keywords,
                                       const QString &file_path) {
  current_keywords_ = keywords;
  current_plain_text_ = text;
  current_file_path_ = file_path;
  has_plain_text_ = true;
  html_mode_ = false;
  is_code_document_ = IsCodeOrStructuredFile(file_path);
  match_ranges_.clear();
  current_match_idx_ = -1;

  UpdateKeywordBadge(keywords);

  const bool want_kp =
      !is_code_document_ && typesetting::PreviewTypographyStore::Instance().config().kp_enabled;
  ApplyActiveViewerMode(want_kp);
}

void DocumentViewerWidget::HighlightOccurrences() {
  match_ranges_.clear();
  current_match_idx_ = -1;
  highlights_truncated_ = false;

  if (current_keywords_.empty() || text_viewer_->toPlainText().isEmpty()) {
    text_viewer_->setExtraSelections({});
    UpdateMatchCountLabel();
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
        highlights_truncated_ = true;
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
    UpdateMatchCountLabel();
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
  if (kp_active_for_doc_ && kp_viewer_) {
    if (kp_viewer_->MatchCount() <= 0) {
      match_count_label_->setText(Tr("match_count").arg(0).arg(0));
      return;
    }
    kp_viewer_->JumpToMatch(index);
    UpdateMatchCountLabel();
    return;
  }

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
  UpdateMatchCountLabel();

  UpdateHighlightSelections();

  const auto &range = match_ranges_[current_match_idx_];
  QTextCursor cursor(text_viewer_->document());
  cursor.setPosition(range.start);
  cursor.setPosition(range.end, QTextCursor::KeepAnchor);
  text_viewer_->setTextCursor(cursor);
  text_viewer_->ensureCursorVisible();
}

void DocumentViewerWidget::FindNext() {
  if (kp_active_for_doc_ && kp_viewer_) {
    const int count = kp_viewer_->MatchCount();
    if (count <= 0) return;
    const int next_idx = (kp_viewer_->CurrentMatchIndex() + 1) % count;
    JumpToMatch(next_idx);
    return;
  }
  if (match_ranges_.empty()) return;
  const int count = static_cast<int>(match_ranges_.size());
  const int next_idx = (current_match_idx_ + 1) % count;
  JumpToMatch(next_idx);
}

void DocumentViewerWidget::FindPrev() {
  if (kp_active_for_doc_ && kp_viewer_) {
    const int count = kp_viewer_->MatchCount();
    if (count <= 0) return;
    const int prev_idx = (kp_viewer_->CurrentMatchIndex() - 1 + count) % count;
    JumpToMatch(prev_idx);
    return;
  }
  if (match_ranges_.empty()) return;
  const int count = static_cast<int>(match_ranges_.size());
  const int prev_idx = (current_match_idx_ - 1 + count) % count;
  JumpToMatch(prev_idx);
}

namespace {

constexpr size_t kMaxPreviewContentBytes = size_t{4} * 1024 * 1024;

bool IsPlainTextFallbackAllowed(const QString &file_path) {
  const QString ext = QFileInfo(file_path).suffix().toLower();
  if (ext.isEmpty()) {
    return true;
  }
  // Structured documents, images, archives, and native binaries must never fall back to raw disk reads
  // in plain-text preview (prevents leaking raw %PDF-/OLE/ZIP bytes when indexed content is empty).
  if (soeasy::common::IsSupportedImageExtension(ext.toStdString()) ||
      soeasy::common::IsSupportedArchiveExtension(ext.toStdString())) {
    return false;
  }
  static const std::unordered_set<std::string> kBlockedBinaryOrParsedExts = {
      "pdf", "doc",   "docx",   "xls",     "xlsx",    "xlsb",  "ppt", "pptx",   "odt", "ods", "odp", "odg",
      "fodt", "fods", "fodp",   "rtf",     "wps",     "et",    "dps", "pages",  "key", "keynote", "numbers",
      "eml",  "gif",  "ico",    "svg",     "exe",     "dll",   "so",  "dylib",  "bin", "obj", "o",   "a",
      "lib",  "db",   "sqlite", "sqlite3", "class",   "pyc",   "wasm","msi",    "cab", "iso", "img", "dmg",
      "deb",  "rpm",
  };
  return kBlockedBinaryOrParsedExts.find(ext.toStdString()) == kBlockedBinaryOrParsedExts.end();
}

bool HasVisibleText(const std::string &text) {
  return std::any_of(text.begin(), text.end(), [](unsigned char c) { return !std::isspace(c); });
}

bool HasBinaryOrDocumentMagic(const QByteArray &bytes) {
  if (bytes.size() >= 8 &&
      std::memcmp(bytes.constData(), "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0) {
    return true;
  }
  if (bytes.size() >= 4 && std::memcmp(bytes.constData(), "PK\x03\x04", 4) == 0) return true;
  if (bytes.size() >= 4 && std::memcmp(bytes.constData(), "\x89PNG", 4) == 0) return true;
  if (bytes.size() >= 3 && std::memcmp(bytes.constData(), "\xFF\xD8\xFF", 3) == 0) return true;
  if (bytes.size() >= 4 && std::memcmp(bytes.constData(), "GIF8", 4) == 0) return true;
  if (bytes.size() >= 4 && std::memcmp(bytes.constData(), "Rar!", 4) == 0) return true;
  if (bytes.size() >= 6 && std::memcmp(bytes.constData(), "7z\xBC\xAF\x27\x1C", 6) == 0) return true;
  if (bytes.size() >= 2 && std::memcmp(bytes.constData(), "\x1F\x8B", 2) == 0) return true;

  // Skip optional UTF-8 BOM and leading ASCII whitespace (ISO 32000-1 permits leading offset before %PDF-)
  int off = 0;
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
    off = 3;
  }
  const int limit = std::min<int>(bytes.size(), 1024);
  while (off < limit && std::isspace(static_cast<unsigned char>(bytes[off]))) {
    ++off;
  }
  if (bytes.size() - off >= 5 && std::memcmp(bytes.constData() + off, "%PDF-", 5) == 0) return true;
  if (bytes.size() - off >= 5 && std::memcmp(bytes.constData() + off, "{\\rtf", 5) == 0) return true;
  return false;
}

bool FallbackReadFromDisk(const QString &file_path, std::string *out_content) {
  constexpr qint64 kMaxDirectDiskReadBytes = 1024LL * 1024LL;
  QFileInfo fi(file_path);
  if (fi.isFile() && fi.size() > 0) {
    QFile f(file_path);
    if (f.open(QIODevice::ReadOnly)) {
      QByteArray bytes = f.read(kMaxDirectDiskReadBytes);
      if (HasBinaryOrDocumentMagic(bytes)) {
        return false;
      }
      int ctrl_count = 0;
      for (int i = 0; i < bytes.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(bytes[i]);
        if (ch == '\0') {
          return false;
        }
        if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f' && ch != '\b') {
          ++ctrl_count;
        }
      }
      if (!bytes.isEmpty() && ctrl_count * 50 > bytes.size()) {
        return false;
      }
      size_t valid_len = static_cast<size_t>(bytes.size());
      if (fi.size() > kMaxDirectDiskReadBytes && valid_len > 0) {
        size_t pos = valid_len - 1;
        size_t back = 0;
        while (pos > 0 && back < 3 && (static_cast<unsigned char>(bytes[static_cast<int>(pos)]) & 0xC0) == 0x80) {
          --pos;
          ++back;
        }
        const unsigned char lead = static_cast<unsigned char>(bytes[static_cast<int>(pos)]);
        size_t expected_len = 1;
        if ((lead & 0xE0) == 0xC0) {
          expected_len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
          expected_len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
          expected_len = 4;
        }
        if ((expected_len > 1 && (valid_len - pos) < expected_len) || (lead & 0xC0) == 0x80) {
          valid_len = pos;
        }
      }
      out_content->assign(bytes.constData(), valid_len);
      return !out_content->empty();
    }
  }
  return false;
}

void TruncatePreviewContent(std::string *content, bool *out_truncated) {
  if (out_truncated) *out_truncated = false;
  if (!content || content->size() <= kMaxPreviewContentBytes) {
    return;
  }
  size_t cut = kMaxPreviewContentBytes;
  // Never cut in the middle of a UTF-8 multi-byte sequence.
  while (cut > 0 && (static_cast<unsigned char>((*content)[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  content->resize(cut);
  if (out_truncated) *out_truncated = true;
}

}  // namespace

DocumentPreviewResult DocumentViewerWidget::LoadPreviewDocument(
    int64_t doc_id, const QString &file_path, const std::shared_ptr<service::ISearchCoordinator> &coordinator,
    bool allow_disk_fallback) {
  DocumentPreviewResult result;
  try {
    if (coordinator && doc_id > 0) {
      coordinator->GetDocumentParseInfo(doc_id, &result.parse_status, &result.issue_reason);
      if (common::IsIssueParseStatus(result.parse_status) &&
          result.parse_status != common::ParseStatus::kPartialOcr) {
        return result;
      }
      if (coordinator->GetDocumentContent(doc_id, &result.content) && HasVisibleText(result.content)) {
        TruncatePreviewContent(&result.content, &result.truncated);
        result.has_content = !result.content.empty();
        return result;
      }
      result.content.clear();
    }

    if (!allow_disk_fallback || !IsPlainTextFallbackAllowed(file_path)) {
      return result;
    }
    if (FallbackReadFromDisk(file_path, &result.content) && HasVisibleText(result.content)) {
      TruncatePreviewContent(&result.content, &result.truncated);
      result.has_content = !result.content.empty();
      return result;
    }
    result.content.clear();
  } catch (const std::exception &e) {
    LOG_WARN("[DocumentViewer] Failed to read content for '{}': {}", file_path.toStdString(), e.what());
    result.content.clear();
  } catch (...) {
    LOG_WARN("[DocumentViewer] Failed to read content for '{}': unknown error", file_path.toStdString());
    result.content.clear();
  }
  return result;
}

QString DocumentViewerWidget::FormatPreviewText(const DocumentPreviewResult &preview) {
  QString text = QString::fromStdString(preview.content);
  if (preview.truncated) {
    text.append("\n\n");
    text.append(Tr("preview_content_truncated"));
  }
  return text;
}

QString DocumentViewerWidget::BuildStatusNoticeHtml(const DocumentPreviewResult &preview) {
  auto format_notice = [&](const QString &bg, const QString &border, const QString &fg, const QString &title,
                           const QString &detail) {
    QString html = QString(
                       "<div style='background:%1; border:1px solid %2; border-radius:6px; padding:12px 14px; "
                       "color:%3; font-size:12px; line-height:1.6;'>"
                       "<div style='font-weight:600; margin-bottom:4px;'>%4</div>")
                       .arg(bg, border, fg, title.toHtmlEscaped());
    if (!detail.isEmpty()) {
      html += QString("<div style='color:#475569; font-size:11px;'>%1</div>").arg(detail.toHtmlEscaped());
    }
    html += "</div>";
    return html;
  };

  const QString reason = QString::fromStdString(preview.issue_reason).trimmed();
  const QString reason_line = reason.isEmpty() ? QString() : (Tr("preview_reason_label") + reason);

  switch (preview.parse_status) {
    case common::ParseStatus::kEncrypted: {
      QString detail = Tr("preview_encrypted_desc");
      if (!reason.isEmpty()) {
        detail += QString(" (%1)").arg(reason);
      }
      return format_notice("#FFFBEB", "#FDE68A", "#92400E", Tr("preview_encrypted_title"), detail);
    }
    case common::ParseStatus::kTimeout:
      return format_notice("#FFF7ED", "#FED7AA", "#9A3412", Tr("preview_timeout_title"), reason_line);
    case common::ParseStatus::kCrashed:
      return format_notice("#FEF2F2", "#FECACA", "#991B1B", Tr("preview_crashed_title"), reason_line);
    case common::ParseStatus::kFailed:
      return format_notice("#FEF2F2", "#FECACA", "#991B1B", Tr("preview_failed_title"), reason_line);
    case common::ParseStatus::kPartialOcr:
      return format_notice("#F0F9FF", "#BAE6FD", "#0369A1", Tr("preview_partial_ocr_title"), reason_line);
    case common::ParseStatus::kEmptyContent:
      return format_notice("#F8FAFC", "#CBD5E1", "#475569", Tr("preview_empty_content_title"), reason_line);
    case common::ParseStatus::kOk:
      return QString("<p style='color:#64748B; font-style:italic;'>%1</p>")
          .arg(Tr("preview_empty_parsed_doc").toHtmlEscaped());
    default:
      return QString("<p style='color:#94A3B8; font-style:italic;'>%1</p>")
          .arg(Tr("preview_cannot_read").toHtmlEscaped());
  }
}

bool DocumentViewerWidget::ReadDocumentContent(int64_t doc_id, const QString &file_path,
                                               const std::shared_ptr<service::ISearchCoordinator> &coordinator,
                                               std::string *out_content) {
  if (!out_content) return false;
  out_content->clear();

  DocumentPreviewResult preview = LoadPreviewDocument(doc_id, file_path, coordinator);
  if (!preview.has_content || preview.content.empty()) {
    return false;
  }
  *out_content = std::move(preview.content);
  return true;
}

}  // namespace gui
}  // namespace soeasy
