#ifndef SOEASY_SRC_GUI_ICON_PROVIDER_H_
#define SOEASY_SRC_GUI_ICON_PROVIDER_H_

#include <QIcon>
#include <QString>

namespace soeasy {
namespace gui {

class IconProvider {
 public:
  static QIcon GetFileIcon(const QString& extension_or_filename);
  static QIcon GetSearchIcon();
  static QIcon GetAppIcon();
  static QString GetDownArrowImagePath();

 private:
  static QIcon RenderFileBadgeIcon(const QString& badge_text,
                                   const QString& bg_color_hex,
                                   bool fold_corner = true);
  static QIcon RenderTextDocumentIcon();
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_ICON_PROVIDER_H_
