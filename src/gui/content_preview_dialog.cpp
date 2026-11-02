#include "gui/content_preview_dialog.h"
#include "gui/i18n.h"

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

namespace soeasy {
namespace gui {

ContentPreviewDialog::ContentPreviewDialog(
    int64_t doc_id, const QString& file_name, const QString& file_path,
    const std::vector<std::string>& keywords,
    std::shared_ptr<storage::StorageEngine> storage, QWidget* parent)
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
      "  font-family: 'Microsoft YaHei UI', 'Microsoft YaHei', 'Segoe UI', 'PingFang SC', 'Noto Sans CJK SC', 'WenQuanYi Micro Hei', sans-serif;"
      "}");

  auto* main_layout = new QVBoxLayout(this);
  main_layout->setSpacing(8);

  // Top header row: File path and actions
  auto* top_layout = new QHBoxLayout();
  path_label_ = new QLabel(file_path_, this);
  path_label_->setStyleSheet("font-size: 12px; color: #546E7A; font-family: monospace;");
  path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto* btn_open_folder = new QPushButton(Tr("btn_open_folder_sys"), this);
  auto* btn_open_app = new QPushButton(Tr("btn_open_app_sys"), this);
  btn_open_folder->setStyleSheet("padding: 4px 10px;");
  btn_open_app->setStyleSheet("padding: 4px 10px;");

  top_layout->addWidget(path_label_, 1);
  top_layout->addWidget(btn_open_folder);
  top_layout->addWidget(btn_open_app);
  main_layout->addLayout(top_layout);

  connect(btn_open_folder, &QPushButton::clicked, this,
          &ContentPreviewDialog::OnOpenFolder);
  connect(btn_open_app, &QPushButton::clicked, this,
          &ContentPreviewDialog::OnOpenWithDefaultApp);

  // Search/Navigation bar inside document
  auto* nav_layout = new QHBoxLayout();
  auto* label_search = new QLabel(Tr("label_jump"), this);
  in_doc_search_edit_ = new QLineEdit(this);
  if (!keywords_.empty()) {
    in_doc_search_edit_->setText(QString::fromStdString(keywords_[0]));
  }
  in_doc_search_edit_->setPlaceholderText(Tr("search_in_content_ph"));
  in_doc_search_edit_->setMaximumWidth(200);

  auto* btn_prev = new QPushButton(Tr("btn_prev"), this);
  auto* btn_next = new QPushButton(Tr("btn_next"), this);
  match_count_label_ = new QLabel(Tr("match_count").arg(0).arg(0), this);
  match_count_label_->setStyleSheet("color: #78909C; font-weight: bold;");

  nav_layout->addWidget(label_search);
  nav_layout->addWidget(in_doc_search_edit_);
  nav_layout->addWidget(btn_prev);
  nav_layout->addWidget(btn_next);
  nav_layout->addWidget(match_count_label_);
  nav_layout->addStretch();
  main_layout->addLayout(nav_layout);

  connect(btn_next, &QPushButton::clicked, this,
          &ContentPreviewDialog::OnFindNext);
  connect(btn_prev, &QPushButton::clicked, this,
          &ContentPreviewDialog::OnFindPrev);
  connect(in_doc_search_edit_, &QLineEdit::textChanged, this, [this]() {
    keywords_.clear();
    QString text = in_doc_search_edit_->text().trimmed();
    if (!text.isEmpty()) {
      keywords_.push_back(text.toStdString());
    }
    HighlightOccurrences();
  });

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
    decompress_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
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
    text_viewer_->setHtml(
        "<p style='color:#B0BEC5; font-style:italic;'>无法读取文档内容或文档为空。</p>");
    return;
  }

  // Cap maximum displayed characters to protect Qt rich-text layout engine from hanging
  constexpr size_t kMaxDisplayChars = 2 * 1024 * 1024;  // 2 MB
  if (decompressed_utf8.size() > kMaxDisplayChars) {
    decompressed_utf8.resize(kMaxDisplayChars);
    decompressed_utf8.append("\n\n[... 内容过长，已截断展示 ...]");
  }

  if (decompress_ms > 0.0 && path_label_) {
    path_label_->setToolTip(
        QString("文件路径: %1\n密态解密耗时: %2 ms")
            .arg(file_path_)
            .arg(decompress_ms, 0, 'f', 2));
  }

  text_viewer_->setPlainText(QString::fromStdString(decompressed_utf8));
  HighlightOccurrences();
}

void ContentPreviewDialog::HighlightOccurrences() {
  match_positions_.clear();
  current_match_idx_ = -1;

  if (keywords_.empty() || text_viewer_->toPlainText().isEmpty()) {
    match_count_label_->setText("匹配: 0 / 0");
    return;
  }

  QTextDocument* doc = text_viewer_->document();
  QTextCharFormat highlight_format;
  highlight_format.setBackground(QColor("#FFF59D"));
  highlight_format.setForeground(QColor("#B71C1C"));
  highlight_format.setFontWeight(QFont::Bold);

  // Clear existing formatting
  QTextCursor clear_cursor(doc);
  clear_cursor.select(QTextCursor::Document);
  QTextCharFormat default_format;
  clear_cursor.setCharFormat(default_format);

  for (const auto& kw : keywords_) {
    if (kw.empty()) continue;
    QString query_qstr = QString::fromStdString(kw);

    QTextCursor cursor(doc);
    while (true) {
      cursor = doc->find(query_qstr, cursor, QTextDocument::FindFlags());
      if (cursor.isNull()) {
        break;
      }
      match_positions_.push_back(cursor.selectionStart());
      cursor.mergeCharFormat(highlight_format);
    }
  }

  // Sort match positions
  std::sort(match_positions_.begin(), match_positions_.end());
  match_positions_.erase(
      std::unique(match_positions_.begin(), match_positions_.end()),
      match_positions_.end());

  if (!match_positions_.empty()) {
    JumpToMatch(0);
  } else {
    match_count_label_->setText(Tr("match_count").arg(0).arg(0));
  }
}

void ContentPreviewDialog::JumpToMatch(int index) {
  if (match_positions_.empty()) {
    match_count_label_->setText(Tr("match_count").arg(0).arg(0));
    return;
  }

  if (index < 0) {
    index = static_cast<int>(match_positions_.size()) - 1;
  } else if (index >= static_cast<int>(match_positions_.size())) {
    index = 0;
  }

  current_match_idx_ = index;
  int pos = match_positions_[current_match_idx_];

  QTextCursor cursor(text_viewer_->document());
  cursor.setPosition(pos);
  text_viewer_->setTextCursor(cursor);
  text_viewer_->ensureCursorVisible();

  match_count_label_->setText(
      Tr("match_count").arg(current_match_idx_ + 1).arg(match_positions_.size()));
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
