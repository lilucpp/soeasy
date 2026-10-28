#include "gui/icon_provider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace soeasy {
namespace gui {

namespace {

// In-memory cache for rendered icons
static QMap<QString, QIcon> s_icon_cache;

}  // namespace

QIcon IconProvider::RenderFileBadgeIcon(const QString& badge_text,
                                        const QString& bg_color_hex,
                                        bool fold_corner) {
  const int size = 32;
  QPixmap pix(size, size);
  pix.fill(Qt::transparent);

  QPainter p(&pix);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);

  const qreal x0 = 4.0;
  const qreal y0 = 3.0;
  const qreal w = 24.0;
  const qreal h = 26.0;
  const qreal fold = fold_corner ? 7.0 : 0.0;

  QPainterPath path;
  if (fold_corner) {
    path.moveTo(x0, y0 + 3.0);
    path.quadTo(x0, y0, x0 + 3.0, y0);
    path.lineTo(x0 + w - fold, y0);
    path.lineTo(x0 + w, y0 + fold);
    path.lineTo(x0 + w, y0 + h - 3.0);
    path.quadTo(x0 + w, y0 + h, x0 + w - 3.0, y0 + h);
    path.lineTo(x0 + 3.0, y0 + h);
    path.quadTo(x0, y0 + h, x0, y0 + h - 3.0);
    path.closeSubpath();
  } else {
    path.addRoundedRect(x0, y0, w, h, 4.0, 4.0);
  }

  p.setPen(Qt::NoPen);
  p.setBrush(QColor(bg_color_hex));
  p.drawPath(path);

  if (fold_corner) {
    // Draw folded corner flap
    QPainterPath flap;
    flap.moveTo(x0 + w - fold, y0);
    flap.lineTo(x0 + w - fold, y0 + fold);
    flap.lineTo(x0 + w, y0 + fold);
    flap.closeSubpath();

    p.setBrush(QColor(255, 255, 255, 110));
    p.drawPath(flap);
  }

  // Draw badge text (e.g. W, pdf, md, X, P, </>)
  p.setPen(Qt::white);
  QFont f = p.font();
  f.setBold(true);
  if (badge_text.size() <= 1) {
    f.setPixelSize(14);
  } else if (badge_text.size() == 2) {
    f.setPixelSize(11);
  } else {
    f.setPixelSize(9);
  }
  p.setFont(f);

  QRect text_rect(x0, y0 + (fold_corner ? 4 : 0), w, h - (fold_corner ? 4 : 0));
  p.drawText(text_rect, Qt::AlignCenter, badge_text);

  return QIcon(pix);
}

QIcon IconProvider::RenderTextDocumentIcon() {
  const int size = 32;
  QPixmap pix(size, size);
  pix.fill(Qt::transparent);

  QPainter p(&pix);
  p.setRenderHint(QPainter::Antialiasing, true);

  const qreal x0 = 4.0;
  const qreal y0 = 3.0;
  const qreal w = 24.0;
  const qreal h = 26.0;
  const qreal fold = 7.0;

  QPainterPath path;
  path.moveTo(x0, y0 + 3.0);
  path.quadTo(x0, y0, x0 + 3.0, y0);
  path.lineTo(x0 + w - fold, y0);
  path.lineTo(x0 + w, y0 + fold);
  path.lineTo(x0 + w, y0 + h - 3.0);
  path.quadTo(x0 + w, y0 + h, x0 + w - 3.0, y0 + h);
  path.lineTo(x0 + 3.0, y0 + h);
  path.quadTo(x0, y0 + h, x0, y0 + h - 3.0);
  path.closeSubpath();

  // White page with subtle border
  p.setPen(QPen(QColor("#78909C"), 1.2));
  p.setBrush(QColor("#FFFFFF"));
  p.drawPath(path);

  // Folded flap
  QPainterPath flap;
  flap.moveTo(x0 + w - fold, y0);
  flap.lineTo(x0 + w - fold, y0 + fold);
  flap.lineTo(x0 + w, y0 + fold);
  flap.closeSubpath();
  p.setPen(QPen(QColor("#78909C"), 1.0));
  p.setBrush(QColor("#ECEFF1"));
  p.drawPath(flap);

  // Document lines
  p.setPen(QPen(QColor("#90A4AE"), 1.5, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(QPointF(x0 + 5, y0 + 11), QPointF(x0 + w - 5, y0 + 11));
  p.drawLine(QPointF(x0 + 5, y0 + 15), QPointF(x0 + w - 5, y0 + 15));
  p.drawLine(QPointF(x0 + 5, y0 + 19), QPointF(x0 + w - 8, y0 + 19));

  return QIcon(pix);
}

QIcon IconProvider::GetFileIcon(const QString& extension_or_filename) {
  QString ext = extension_or_filename.toLower();
  if (ext.contains('.')) {
    ext = QFileInfo(extension_or_filename).suffix().toLower();
  }

  if (s_icon_cache.contains(ext)) {
    return s_icon_cache[ext];
  }

  QIcon icon;
  if (ext == "doc" || ext == "docx") {
    icon = RenderFileBadgeIcon("W", "#2B579A", true);
  } else if (ext == "pdf") {
    icon = RenderFileBadgeIcon("pdf", "#D32F2F", true);
  } else if (ext == "md" || ext == "markdown") {
    icon = RenderFileBadgeIcon("md", "#0288D1", false);
  } else if (ext == "txt" || ext == "log" || ext == "ini" || ext == "conf") {
    icon = RenderTextDocumentIcon();
  } else if (ext == "xls" || ext == "xlsx" || ext == "csv") {
    icon = RenderFileBadgeIcon("X", "#2E7D32", true);
  } else if (ext == "ppt" || ext == "pptx") {
    icon = RenderFileBadgeIcon("P", "#E65100", true);
  } else if (ext == "cpp" || ext == "c" || ext == "h" || ext == "hpp" ||
             ext == "py" || ext == "java" || ext == "js" || ext == "ts" ||
             ext == "json" || ext == "xml" || ext == "html" || ext == "css") {
    icon = RenderFileBadgeIcon("</>", "#00897B", true);
  } else {
    icon = RenderFileBadgeIcon("", "#90A4AE", true);
  }

  s_icon_cache[ext] = icon;
  return icon;
}

QIcon IconProvider::GetSearchIcon() {
  if (s_icon_cache.contains("__search__")) {
    return s_icon_cache["__search__"];
  }

  const int size = 24;
  QPixmap pix(size, size);
  pix.fill(Qt::transparent);

  QPainter p(&pix);
  p.setRenderHint(QPainter::Antialiasing, true);

  QPen pen(QColor("#64748B"), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);

  // Circle
  p.drawEllipse(QPointF(9.5, 9.5), 5.5, 5.5);
  // Handle
  p.drawLine(QPointF(13.8, 13.8), QPointF(19.0, 19.0));

  QIcon icon(pix);
  s_icon_cache["__search__"] = icon;
  return icon;
}

QIcon IconProvider::GetAppIcon() {
  if (s_icon_cache.contains("__app__")) {
    return s_icon_cache["__app__"];
  }

  const int size = 32;
  QPixmap pix(size, size);
  pix.fill(Qt::transparent);

  QPainter p(&pix);
  p.setRenderHint(QPainter::Antialiasing, true);

  // Folder tab & back
  QPainterPath tab;
  tab.moveTo(3, 8);
  tab.lineTo(12, 8);
  tab.lineTo(14, 11);
  tab.lineTo(28, 11);
  tab.quadTo(29, 11, 29, 12);
  tab.lineTo(29, 26);
  tab.quadTo(29, 27, 28, 27);
  tab.lineTo(3, 27);
  tab.quadTo(2, 27, 2, 26);
  tab.lineTo(2, 9);
  tab.quadTo(2, 8, 3, 8);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor("#F59E0B"));
  p.drawPath(tab);

  // Folder front
  QPainterPath front;
  front.moveTo(2, 13);
  front.lineTo(29, 13);
  front.lineTo(27, 27);
  front.lineTo(4, 27);
  front.closeSubpath();
  p.setBrush(QColor("#FBBF24"));
  p.drawPath(front);

  // Magnifying glass overlay
  QPen glass_pen(QColor("#1E40AF"), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(glass_pen);
  p.setBrush(QColor(255, 255, 255, 180));
  p.drawEllipse(QPointF(17, 19), 6.0, 6.0);
  p.drawLine(QPointF(21.5, 23.5), QPointF(27.0, 29.0));

  QIcon icon(pix);
  s_icon_cache["__app__"] = icon;
  return icon;
}

QString IconProvider::GetDownArrowImagePath() {
  const QString path = QDir::tempPath() + "/soeasy_arrow_down.png";
  if (QFile::exists(path)) {
    return path;
  }
  QPixmap arrow(12, 8);
  arrow.fill(Qt::transparent);
  {
    QPainter p(&arrow);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor("#64748B"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    QPainterPath path_draw;
    path_draw.moveTo(2, 2.5);
    path_draw.lineTo(6, 6);
    path_draw.lineTo(10, 2.5);
    p.drawPath(path_draw);
  }
  arrow.save(path);
  return path;
}

}  // namespace gui
}  // namespace soeasy
