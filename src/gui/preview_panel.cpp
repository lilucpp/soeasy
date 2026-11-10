#include "gui/preview_panel.h"

#include <QApplication>
#include <QClipboard>
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
  setObjectName("PreviewPanel");

  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  // 1. Header Bar: File Info & Quick Actions
  auto *header_bar = new QFrame(this);
  header_bar->setObjectName("PreviewHeaderBar");
  auto *header_layout = new QVBoxLayout(header_bar);
  header_layout->setContentsMargins(16, 12, 16, 10);
  header_layout->setSpacing(6);

  auto *title_row = new QHBoxLayout();
  title_row->setSpacing(10);

  icon_label_ = new QLabel(header_bar);
  icon_label_->setFixedSize(24, 24);
  icon_label_->setAlignment(Qt::AlignCenter);

  title_label_ = new QLabel("未选中文件", header_bar);
  title_label_->setObjectName("PreviewFileTitle");
  QFont f_title = title_label_->font();
  f_title.setBold(true);
  f_title.setPointSize(11);
  title_label_->setFont(f_title);

  btn_open_file_ = new QPushButton("打开文件", header_bar);
  btn_open_file_->setProperty("role", "primary");
  btn_open_folder_ = new QPushButton("所在目录", header_bar);
  btn_open_folder_->setProperty("role", "secondary");

  title_row->addWidget(icon_label_);
  title_row->addWidget(title_label_, 1);
  title_row->addWidget(btn_open_file_);
  title_row->addWidget(btn_open_folder_);
  header_layout->addLayout(title_row);

  auto *path_row = new QHBoxLayout();
  path_row->setSpacing(6);
  path_label_ = new QLabel(header_bar);
  path_label_->setObjectName("PreviewFilePath");
  path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  path_label_->setWordWrap(true);

  btn_copy_path_ = new QPushButton("复制", header_bar);
  btn_copy_path_->setProperty("role", "secondary");
  btn_copy_path_->setFixedSize(40, 22);
  btn_copy_path_->setToolTip("复制完整路径至剪贴板");

  path_row->addWidget(path_label_, 1);
  path_row->addWidget(btn_copy_path_);
  header_layout->addLayout(path_row);

  main_layout->addWidget(header_bar);

  // 2. Navigation / In-Document Search Bar
  auto *nav_bar = new QFrame(this);
  nav_bar->setObjectName("KeywordNavBar");
  auto *nav_layout = new QHBoxLayout(nav_bar);
  nav_layout->setContentsMargins(16, 0, 16, 0);
  nav_layout->setSpacing(8);

  label_jump_ = new QLabel("关键词", nav_bar);
  label_jump_->setProperty("role", "hint-label");

  keyword_pill_ = new QLabel(nav_bar);
  keyword_pill_->setObjectName("KeywordPill");
  keyword_pill_->setVisible(false);

  in_doc_search_edit_ = new QLineEdit(nav_bar);
  in_doc_search_edit_->setPlaceholderText("在正文中查找...");
  in_doc_search_edit_->setObjectName("InDocSearchEdit");
  in_doc_search_edit_->setMaximumWidth(140);

  match_count_label_ = new QLabel("第 0 / 0 处", nav_bar);
  match_count_label_->setObjectName("MatchCountLabel");

  btn_prev_ = new QPushButton("▲", nav_bar);
  btn_prev_->setProperty("role", "secondary");
  btn_prev_->setFixedSize(24, 24);
  btn_prev_->setToolTip("跳转至上一个匹配项");

  btn_next_ = new QPushButton("▼", nav_bar);
  btn_next_->setProperty("role", "secondary");
  btn_next_->setFixedSize(24, 24);
  btn_next_->setToolTip("跳转至下一个匹配项");

  nav_layout->addWidget(label_jump_);
  nav_layout->addWidget(keyword_pill_);
  nav_layout->addWidget(in_doc_search_edit_);
  nav_layout->addStretch();
  nav_layout->addWidget(match_count_label_);
  nav_layout->addWidget(btn_prev_);
  nav_layout->addWidget(btn_next_);
  main_layout->addWidget(nav_bar);

  // 3. Document Content Viewer
  text_viewer_ = new QTextBrowser(this);
  text_viewer_->setReadOnly(true);
  text_viewer_->setObjectName("SourceCodeViewer");
  main_layout->addWidget(text_viewer_, 1);

  // 4. Meta Status Bar (#PreviewFooterBar)
  auto *footer_bar = new QFrame(this);
  footer_bar->setObjectName("PreviewFooterBar");
  auto *footer_layout = new QHBoxLayout(footer_bar);
  footer_layout->setContentsMargins(16, 0, 16, 0);

  meta_label_ = new QLabel(footer_bar);
  meta_label_->setObjectName("PreviewMetaLabel");
  footer_layout->addWidget(meta_label_);
  footer_layout->addStretch();
  main_layout->addWidget(footer_bar);

  // Connections
  connect(btn_open_file_, &QPushButton::clicked, this, &PreviewPanel::OnOpenWithDefaultApp);
  connect(btn_open_folder_, &QPushButton::clicked, this, &PreviewPanel::OnOpenFolder);
  connect(btn_copy_path_, &QPushButton::clicked, this, &PreviewPanel::OnCopyPath);
  connect(btn_next_, &QPushButton::clicked, this, &PreviewPanel::OnFindNext);
  connect(btn_prev_, &QPushButton::clicked, this, &PreviewPanel::OnFindPrev);
  connect(in_doc_search_edit_, &QLineEdit::textChanged, this, [this]() {
    current_keywords_.clear();
    QString text = in_doc_search_edit_->text().trimmed();
    if (!text.isEmpty()) {
      current_keywords_.push_back(text.toStdString());
    }
    HighlightOccurrences();
  });

  RetranslateUi();
}

void PreviewPanel::OnCopyPath() {
  if (!current_file_path_.isEmpty()) {
    QClipboard *cb = QApplication::clipboard();
    if (cb) {
      cb->setText(current_file_path_);
    }
  }
}

void PreviewPanel::RetranslateUi() {
  if (btn_open_file_) {
    btn_open_file_->setText(Tr("btn_open_file"));
  }
  if (btn_open_folder_) {
    btn_open_folder_->setText(Tr("btn_open_folder"));
  }
  if (label_jump_) {
    label_jump_->setText(Tr("label_jump"));
  }
  if (in_doc_search_edit_) {
    in_doc_search_edit_->setPlaceholderText(Tr("search_in_content_ph"));
  }
  if (btn_prev_) {
    btn_prev_->setText(Tr("btn_prev"));
  }
  if (btn_next_) {
    btn_next_->setText(Tr("btn_next"));
  }
  if (match_count_label_) {
    if (match_positions_.empty()) {
      match_count_label_->setText(Tr("match_count").arg(0).arg(0));
    } else {
      int cur = (current_match_idx_ >= 0) ? current_match_idx_ + 1 : 1;
      match_count_label_->setText(Tr("match_count").arg(cur).arg(match_positions_.size()));
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
  match_positions_.clear();
  current_match_idx_ = -1;

  icon_label_->clear();
  title_label_->setText(Tr("no_file_selected"));
  path_label_->setText(Tr("no_content_extracted"));
  btn_open_file_->setEnabled(false);
  btn_open_folder_->setEnabled(false);
  if (btn_copy_path_) btn_copy_path_->setEnabled(false);
  if (keyword_pill_) keyword_pill_->setVisible(false);
  in_doc_search_edit_->clear();
  in_doc_search_edit_->setVisible(true);
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
  if (btn_copy_path_) btn_copy_path_->setEnabled(true);

  if (!keywords.empty() && !keywords[0].empty()) {
    if (keyword_pill_) {
      keyword_pill_->setText(QString::fromStdString(keywords[0]));
      keyword_pill_->setVisible(true);
    }
    in_doc_search_edit_->setVisible(false);
  } else {
    if (keyword_pill_) keyword_pill_->setVisible(false);
    in_doc_search_edit_->setVisible(true);
  }
  in_doc_search_edit_->clear();

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

  QString meta_info = QString("%1  ·  修改于 %2  ·  %3  ·  UTF-8")
                          .arg(FormatFileSize(fi.size()))
                          .arg(fi.lastModified().toString("yyyy-MM-dd hh:mm"))
                          .arg(ext.isEmpty() ? "TEXT" : ext.toUpper());
  if (decompress_ms > 0.0) {
    meta_info += QString("  ·  解密: %1ms").arg(decompress_ms, 0, 'f', 1);
  }
  meta_label_->setText(meta_info);
}

void PreviewPanel::HighlightOccurrences() {
  match_positions_.clear();
  current_match_idx_ = -1;

  if (current_keywords_.empty() || text_viewer_->toPlainText().isEmpty()) {
    match_count_label_->setText("第 0 / 0 处");
    return;
  }

  QTextDocument *doc = text_viewer_->document();
  QTextCharFormat highlight_format;
  highlight_format.setBackground(QColor("#FEF08A"));
  highlight_format.setForeground(QColor("#854D0E"));
  highlight_format.setForeground(QColor("#B71C1C"));
  highlight_format.setFontWeight(QFont::Bold);

  // Clear existing formatting efficiently within edit block
  QTextCursor clear_cursor(doc);
  clear_cursor.beginEditBlock();
  clear_cursor.select(QTextCursor::Document);
  QTextCharFormat default_format;
  clear_cursor.setCharFormat(default_format);
  clear_cursor.endEditBlock();

  const size_t kMaxHighlights = 100;
  QTextCursor edit_cursor(doc);
  edit_cursor.beginEditBlock();

  for (const auto &kw : current_keywords_) {
    if (kw.empty()) continue;
    QString query_qstr = QString::fromStdString(kw);

    QTextCursor cursor(doc);
    while (true) {
      cursor = doc->find(query_qstr, cursor);
      if (cursor.isNull()) break;

      match_positions_.push_back(cursor.position());
      cursor.mergeCharFormat(highlight_format);
      if (match_positions_.size() >= kMaxHighlights) {
        break;
      }
    }
    if (match_positions_.size() >= kMaxHighlights) {
      break;
    }
  }

  edit_cursor.endEditBlock();

  if (!match_positions_.empty()) {
    std::sort(match_positions_.begin(), match_positions_.end());
    match_count_label_->setText(Tr("match_count")
                                    .arg(1)
                                    .arg(QString("%1%2")
                                             .arg(match_positions_.size())
                                             .arg(match_positions_.size() >= kMaxHighlights ? "+" : "")));
    JumpToMatch(0);
  } else {
    match_count_label_->setText(Tr("match_count").arg(0).arg(0));
  }
}

void PreviewPanel::JumpToMatch(int index) {
  if (index < 0 || index >= static_cast<int>(match_positions_.size())) {
    return;
  }

  current_match_idx_ = index;
  match_count_label_->setText(Tr("match_count").arg(current_match_idx_ + 1).arg(match_positions_.size()));

  int pos = match_positions_[current_match_idx_];
  QTextCursor cursor = text_viewer_->textCursor();
  cursor.setPosition(pos);
  text_viewer_->setTextCursor(cursor);
  text_viewer_->ensureCursorVisible();
}

void PreviewPanel::OnFindNext() {
  if (match_positions_.empty()) return;
  int next_idx = (current_match_idx_ + 1) % match_positions_.size();
  JumpToMatch(next_idx);
}

void PreviewPanel::OnFindPrev() {
  if (match_positions_.empty()) return;
  int prev_idx = (current_match_idx_ - 1 + match_positions_.size()) % match_positions_.size();
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
