#include "gui/preview_panel.h"
#include "gui/icon_provider.h"

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

PreviewPanel::PreviewPanel(QWidget* parent) : QWidget(parent) {
  SetupUi();
  ClearPreview();
}

void PreviewPanel::SetStorageEngine(
    std::shared_ptr<storage::StorageEngine> storage) {
  storage_ = std::move(storage);
}

void PreviewPanel::SetupUi() {
  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(8, 0, 0, 0);
  main_layout->setSpacing(6);

  // 1. Header Card: File Info & Quick Actions
  auto* header_card = new QWidget(this);
  header_card->setObjectName("PreviewHeaderCard");
  header_card->setStyleSheet(
      "#PreviewHeaderCard {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "}");
  auto* header_layout = new QVBoxLayout(header_card);
  header_layout->setContentsMargins(10, 8, 10, 8);
  header_layout->setSpacing(4);

  auto* title_row = new QHBoxLayout();
  title_row->setSpacing(8);

  icon_label_ = new QLabel(header_card);
  icon_label_->setFixedSize(24, 24);
  icon_label_->setAlignment(Qt::AlignCenter);
  icon_label_->setStyleSheet("border: none; background: transparent; padding: 0px;");

  title_label_ = new QLabel("未选中文件", header_card);
  title_label_->setStyleSheet(
      "font-size: 13px; font-weight: bold; color: #1E293B; border: none;");

  btn_open_file_ = new QPushButton("打开文件", header_card);
  btn_open_file_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 4px 10px;"
      "  border: 1px solid #7DD3FC;"
      "  border-radius: 4px;"
      "  background-color: #BAE6FD;"
      "  color: #0369A1;"
      "  font-weight: 500;"
      "}"
      "QPushButton:hover { background-color: #93C5FD; }"
      "QPushButton:pressed { background-color: #7DD3FC; }");

  btn_open_folder_ = new QPushButton("所在目录", header_card);
  btn_open_folder_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 4px 10px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #DCE8F5;"
      "  color: #1E293B;"
      "}"
      "QPushButton:hover { background-color: #CBDCEF; }"
      "QPushButton:pressed { background-color: #B8CFE5; }");

  title_row->addWidget(icon_label_);
  title_row->addWidget(title_label_, 1);
  title_row->addWidget(btn_open_file_);
  title_row->addWidget(btn_open_folder_);
  header_layout->addLayout(title_row);

  path_label_ = new QLabel(header_card);
  path_label_->setStyleSheet(
      "font-size: 11px; color: #64748B; border: none; font-family: monospace;");
  path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  path_label_->setWordWrap(true);
  header_layout->addWidget(path_label_);

  main_layout->addWidget(header_card);

  // 2. Navigation / In-Document Search Bar
  auto* nav_layout = new QHBoxLayout();
  nav_layout->setSpacing(6);

  auto* label_jump = new QLabel("关键词跳转:", this);
  label_jump->setStyleSheet("font-size: 12px; color: #475569; font-weight: 500;");

  in_doc_search_edit_ = new QLineEdit(this);
  in_doc_search_edit_->setPlaceholderText("在正文中查找...");
  in_doc_search_edit_->setStyleSheet(
      "QLineEdit {"
      "  font-size: 12px;"
      "  padding: 4px 8px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  color: #1E293B;"
      "}"
      "QLineEdit:focus { border: 1.5px solid #0284C7; }");

  btn_prev_ = new QPushButton("▲ 上一个", this);
  btn_prev_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 3px 8px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #DCE8F5;"
      "  color: #1E293B;"
      "}"
      "QPushButton:hover { background-color: #CBDCEF; }"
      "QPushButton:pressed { background-color: #B8CFE5; }");
  btn_next_ = new QPushButton("▼ 下一个", this);
  btn_next_->setStyleSheet(
      "QPushButton {"
      "  font-size: 11px;"
      "  padding: 3px 8px;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #DCE8F5;"
      "  color: #1E293B;"
      "}"
      "QPushButton:hover { background-color: #CBDCEF; }"
      "QPushButton:pressed { background-color: #B8CFE5; }");

  match_count_label_ = new QLabel("匹配: 0 / 0", this);
  match_count_label_->setStyleSheet("font-size: 11px; color: #64748B; font-weight: bold;");

  nav_layout->addWidget(label_jump);
  nav_layout->addWidget(in_doc_search_edit_, 1);
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
      "  font-size: 12.5px;"
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
  connect(btn_open_file_, &QPushButton::clicked, this,
          &PreviewPanel::OnOpenWithDefaultApp);
  connect(btn_open_folder_, &QPushButton::clicked, this,
          &PreviewPanel::OnOpenFolder);
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
}

void PreviewPanel::ClearPreview() {
  current_doc_id_ = 0;
  current_file_name_.clear();
  current_file_path_.clear();
  current_keywords_.clear();
  match_positions_.clear();
  current_match_idx_ = -1;

  icon_label_->clear();
  title_label_->setText("未选中文件");
  path_label_->setText("请在左侧列表中选择文件以查看正文预览。");
  btn_open_file_->setEnabled(false);
  btn_open_folder_->setEnabled(false);
  in_doc_search_edit_->clear();
  match_count_label_->setText("匹配: 0 / 0");

  text_viewer_->clear();
  meta_label_->clear();
}

void PreviewPanel::LoadDocument(int64_t doc_id, const QString& file_name,
                                const QString& file_path,
                                const std::vector<std::string>& keywords) {
  current_doc_id_ = doc_id;
  current_file_name_ = file_name;
  current_file_path_ = file_path;
  current_keywords_ = keywords;

  QFileInfo fi(file_path);
  QString ext = fi.suffix();

  icon_label_->setPixmap(
      IconProvider::GetFileIcon(ext).pixmap(24, 24));
  title_label_->setText(file_name);
  path_label_->setText(file_path);

  btn_open_file_->setEnabled(true);
  btn_open_folder_->setEnabled(true);

  if (!keywords.empty()) {
    in_doc_search_edit_->setText(QString::fromStdString(keywords[0]));
  } else {
    in_doc_search_edit_->clear();
  }

  std::string decompressed_utf8;
  bool ok = false;
  double decompress_ms = 0.0;

  if (storage_ && doc_id > 0) {
    auto t0 = std::chrono::high_resolution_clock::now();
    ok = storage_->GetDocumentContent(doc_id, &decompressed_utf8);
    auto t1 = std::chrono::high_resolution_clock::now();
    decompress_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
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
    text_viewer_->setHtml(
        "<p style='color:#94A3B8; font-style:italic;'>无法读取文档内容或正文为空。</p>");
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
  match_positions_.clear();
  current_match_idx_ = -1;

  if (current_keywords_.empty() || text_viewer_->toPlainText().isEmpty()) {
    match_count_label_->setText("匹配: 0 / 0");
    return;
  }

  QTextDocument* doc = text_viewer_->document();
  QTextCharFormat highlight_format;
  highlight_format.setBackground(QColor("#FFF59D"));
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

  for (const auto& kw : current_keywords_) {
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
    match_count_label_->setText(
        QString("匹配: 1 / %1%2")
            .arg(match_positions_.size())
            .arg(match_positions_.size() >= kMaxHighlights ? "+" : ""));
    JumpToMatch(0);
  } else {
    match_count_label_->setText("匹配: 0 / 0");
  }
}

void PreviewPanel::JumpToMatch(int index) {
  if (index < 0 || index >= static_cast<int>(match_positions_.size())) {
    return;
  }

  current_match_idx_ = index;
  match_count_label_->setText(
      QString("匹配: %1 / %2")
          .arg(current_match_idx_ + 1)
          .arg(match_positions_.size()));

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
  int prev_idx = (current_match_idx_ - 1 + match_positions_.size()) %
                 match_positions_.size();
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
