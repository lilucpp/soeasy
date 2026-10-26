#ifndef SOEASY_SRC_GUI_HOVER_PREVIEW_POPUP_H_
#define SOEASY_SRC_GUI_HOVER_PREVIEW_POPUP_H_

#include <QLabel>
#include <QPoint>
#include <QString>
#include <QWidget>

namespace soeasy {
namespace gui {

class HoverPreviewPopup : public QWidget {
  Q_OBJECT

 public:
  explicit HoverPreviewPopup(QWidget* parent = nullptr);
  ~HoverPreviewPopup() override = default;

  /**
   * @brief Display hover card next to cursor with keyword highlighted snippet.
   */
  void ShowSnippet(const QPoint& global_pos, const QString& title,
                   const QString& path, const QString& snippet_html,
                   uint64_t file_size, int64_t mtime, double score);

  /**
   * @brief Dismiss the hover preview card.
   */
  void HidePopup();

 private:
  QLabel* title_label_ = nullptr;
  QLabel* path_label_ = nullptr;
  QLabel* snippet_label_ = nullptr;
  QLabel* meta_label_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_HOVER_PREVIEW_POPUP_H_
