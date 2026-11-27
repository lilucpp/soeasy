// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_TYPESETTING_PREVIEW_TYPOGRAPHY_H_
#define SOEASY_SRC_GUI_TYPESETTING_PREVIEW_TYPOGRAPHY_H_

#include <QColor>
#include <QFont>
#include <QObject>
#include <QString>
#include <memory>

#include "gui/typesetting/cjk_typesetter.h"

namespace soeasy {
namespace gui {
namespace typesetting {

struct PreviewTypographyConfig {
  bool kp_enabled = true;
  QString font_choice = "lxgw-wenkai";  // "lxgw-wenkai", "wenyuan-sans", "system"
  int font_size_px = 20;
  bool auto_width = false;
  double text_width_em = 22.0;          // fixed width in em, or max reading width when auto_width == true
  double indent_em = 2.0;
  double par_skip_em = 0.5;
  double line_skip = 1.4;               // Fixed Chen Shuo LINE_SKIP = 1.4
  QString hyphen_lang = "en_US";        // "en_US" or ""
  int runt_penalty = 0;
  int looseness = 0;                    // Fixed TeX looseness = 0
  bool justify = true;
  bool show_guides = true;
  QString theme_preset = "sepia";       // "light", "sepia", "dark", "custom"
  QColor fg_color = QColor("#292524");
  QColor bg_color = QColor("#FAF6EE");

  bool operator==(const PreviewTypographyConfig &other) const;
  bool operator!=(const PreviewTypographyConfig &other) const { return !(*this == other); }

  static void ApplyThemePreset(const QString &preset, PreviewTypographyConfig *cfg);
};

/**
 * @brief Shared typography config store + bundled font / hyphen dictionary loader.
 */
class PreviewTypographyStore : public QObject {
  Q_OBJECT

 public:
  static PreviewTypographyStore &Instance();

  const PreviewTypographyConfig &config() const { return config_; }
  void UpdateConfig(const PreviewTypographyConfig &cfg);
  void ResetDefaults();

  // Resolves and caches bundled QFont (WenYuanSansSC / LXGWWenKaiLite) or system fallback.
  QFont ResolveFont(const PreviewTypographyConfig &cfg) const;
  // Shared RAII Hyphenator loaded from bundled hyph_en_US.dic (nullptr if dictionary unavailable).
  const Hyphenator *GetHyphenator(const QString &lang) const;

  static QString FindFontFile(const QString &file_name);
  static QString FindHyphenDictFile(const QString &lang);

 signals:
  void ConfigChanged(const soeasy::gui::typesetting::PreviewTypographyConfig &cfg);

 private:
  PreviewTypographyStore();
  void Load();
  void Save() const;
  QString ConfigFilePath() const;

  PreviewTypographyConfig config_;
  mutable QString wenyuan_family_;
  mutable QString lxgw_family_;
  mutable bool fonts_probed_ = false;
  mutable std::unique_ptr<Hyphenator> en_us_hyphen_;
  mutable bool hyphen_probed_ = false;
};

}  // namespace typesetting
}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_TYPESETTING_PREVIEW_TYPOGRAPHY_H_
