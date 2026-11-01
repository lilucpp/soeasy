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

  QIcon icon;
  for (int size : {16, 20, 24, 32, 48, 64, 128}) {
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const double s = size / 128.0;

    // 1. Squircle background with Azure to Oceanic Navy gradient
    QLinearGradient bgGrad(0, 0, size, size);
    bgGrad.setColorAt(0.0, QColor("#0284C7"));
    bgGrad.setColorAt(0.45, QColor("#0369A1"));
    bgGrad.setColorAt(1.0, QColor("#0F172A"));

    QRectF squircleRect(8 * s, 8 * s, 112 * s, 112 * s);
    p.setPen(Qt::NoPen);
    p.setBrush(bgGrad);
    p.drawRoundedRect(squircleRect, 26 * s, 26 * s);

    // 2. Snow ambient border
    QPen rimPen(QColor(255, 250, 250, 80), 1.5 * s);
    p.setPen(rimPen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(9 * s, 9 * s, 110 * s, 110 * s), 25 * s, 25 * s);

    // 3. Magnifier glass backing
    QRadialGradient glassGrad(56 * s, 54 * s, 28 * s);
    glassGrad.setColorAt(0.0, QColor(255, 250, 250, 60));
    glassGrad.setColorAt(1.0, QColor(56, 189, 248, 15));
    p.setPen(Qt::NoPen);
    p.setBrush(glassGrad);
    p.drawEllipse(QPointF(56 * s, 54 * s), 28 * s, 28 * s);

    // 4. Magnifier handle
    QPen handleOuter(QColor("#FFFAFA"), 9 * s, Qt::SolidLine, Qt::RoundCap);
    p.setPen(handleOuter);
    p.drawLine(QPointF(77 * s, 75 * s), QPointF(101 * s, 99 * s));

    QPen handleInner(QColor("#0284C7"), 3.5 * s, Qt::SolidLine, Qt::RoundCap);
    p.setPen(handleInner);
    p.drawLine(QPointF(78 * s, 76 * s), QPointF(94 * s, 92 * s));

    // 5. Magnifier outer Snow rim
    QPen lensRim(QColor("#FFFAFA"), 7 * s);
    p.setPen(lensRim);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(56 * s, 54 * s), 28 * s, 28 * s);

    // 6. Modern 'S' Monogram
    QPainterPath sPath;
    sPath.moveTo(68 * s, 40.5 * s);
    sPath.cubicTo(61 * s, 33.5 * s, 45 * s, 35 * s, 44 * s, 45.5 * s);
    sPath.cubicTo(43 * s, 56 * s, 69 * s, 52.5 * s, 68 * s, 63 * s);
    sPath.cubicTo(67 * s, 73 * s, 50 * s, 74.5 * s, 44 * s, 67.5 * s);

    QPen sPen(QColor("#FFFAFA"), 6.5 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(sPen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(sPath);

    // 7. Amber Jewel Dot (Search Target Match)
    p.setPen(QPen(QColor("#FFFAFA"), 1.5 * s));
    p.setBrush(QColor("#F59E0B"));
    p.drawEllipse(QPointF(70 * s, 38 * s), 4.5 * s, 4.5 * s);

    icon.addPixmap(pix);
  }

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
