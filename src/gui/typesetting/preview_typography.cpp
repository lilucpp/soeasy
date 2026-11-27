// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/typesetting/preview_typography.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

#include "common/logger.h"

namespace soeasy {
namespace gui {
namespace typesetting {

namespace {

QStringList CandidateResourceDirs(const QString &sub_dir) {
  QStringList dirs;
  const QString app_dir = QCoreApplication::applicationDirPath();
  if (!app_dir.isEmpty()) {
    dirs << (app_dir + "/" + sub_dir)
         << (app_dir + "/../" + sub_dir)
         << (app_dir + "/../resources/" + sub_dir)
         << (app_dir + "/../../resources/" + sub_dir);
  }
#ifdef SOEASY_PROJECT_ROOT_DIR
  dirs << (QString::fromUtf8(SOEASY_PROJECT_ROOT_DIR) + "/resources/" + sub_dir);
  if (sub_dir == "dict") {
    dirs << (QString::fromUtf8(SOEASY_PROJECT_ROOT_DIR) + "/third_party/hyphen");
  }
#endif
  dirs << ("/opt/soeasy/" + sub_dir)
       << ("/usr/share/soeasy/" + sub_dir)
       << ("/usr/local/share/soeasy/" + sub_dir);
  return dirs;
}

QString LoadBundledFontFamily(const QString &file_name) {
  const QString path = PreviewTypographyStore::FindFontFile(file_name);
  if (path.isEmpty()) {
    return {};
  }
  const int id = QFontDatabase::addApplicationFont(path);
  if (id < 0) {
    LOG_WARN("[Typography] Failed to register bundled font: {}", path.toStdString());
    return {};
  }
  const QStringList families = QFontDatabase::applicationFontFamilies(id);
  return families.isEmpty() ? QString() : families.first();
}

}  // namespace

bool PreviewTypographyConfig::operator==(const PreviewTypographyConfig &other) const {
  return kp_enabled == other.kp_enabled && font_choice == other.font_choice &&
         font_size_px == other.font_size_px && auto_width == other.auto_width &&
         std::abs(text_width_em - other.text_width_em) < 1e-6 &&
         std::abs(indent_em - other.indent_em) < 1e-6 &&
         std::abs(par_skip_em - other.par_skip_em) < 1e-6 &&
         std::abs(line_skip - other.line_skip) < 1e-6 && hyphen_lang == other.hyphen_lang &&
         runt_penalty == other.runt_penalty && looseness == other.looseness && justify == other.justify &&
         show_guides == other.show_guides && theme_preset == other.theme_preset &&
         fg_color == other.fg_color && bg_color == other.bg_color;
}

void PreviewTypographyConfig::ApplyThemePreset(const QString &preset, PreviewTypographyConfig *cfg) {
  if (!cfg) return;
  cfg->theme_preset = preset;
  if (preset == "sepia") {
    cfg->fg_color = QColor("#292524");
    cfg->bg_color = QColor("#FAF6EE");
  } else if (preset == "dark") {
    cfg->fg_color = QColor("#E2E8F0");
    cfg->bg_color = QColor("#0F172A");
  } else if (preset == "light") {
    cfg->fg_color = QColor("#1E293B");
    cfg->bg_color = QColor("#FFFFFF");
  }
}

PreviewTypographyStore &PreviewTypographyStore::Instance() {
  static PreviewTypographyStore store;
  return store;
}

PreviewTypographyStore::PreviewTypographyStore() { Load(); }

QString PreviewTypographyStore::ConfigFilePath() const {
  QString app_data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (app_data.isEmpty()) {
    app_data = QDir::homePath() + "/.soeasy";
  }
  QDir().mkpath(app_data);
  return app_data + "/preview.ini";
}

void PreviewTypographyStore::Load() {
  QSettings settings(ConfigFilePath(), QSettings::IniFormat);
  settings.beginGroup("knuth_plass");
  config_.kp_enabled = settings.value("kp_enabled", true).toBool();
  config_.font_choice = settings.value("font_choice", "lxgw-wenkai").toString();
  config_.font_size_px = std::clamp(settings.value("font_size_px", 20).toInt(), 11, 36);
  config_.auto_width = settings.value("auto_width", false).toBool();
  config_.text_width_em = std::clamp(settings.value("text_width_em", 22.0).toDouble(), 8.0, 80.0);
  config_.indent_em = std::clamp(settings.value("indent_em", 2.0).toDouble(), 0.0, 10.0);
  config_.par_skip_em = std::clamp(settings.value("par_skip_em", 0.5).toDouble(), 0.0, 5.0);
  config_.line_skip = 1.4;
  config_.hyphen_lang = settings.value("hyphen_lang", "en_US").toString();
  config_.runt_penalty = std::clamp(settings.value("runt_penalty", 0).toInt(), 0, 1000);
  config_.looseness = 0;
  config_.justify = settings.value("justify", true).toBool();
  config_.show_guides = settings.value("show_guides", true).toBool();
  config_.theme_preset = settings.value("theme_preset", "sepia").toString();
  QColor fg(settings.value("fg_color", "#292524").toString());
  QColor bg(settings.value("bg_color", "#FAF6EE").toString());
  if (fg.isValid()) config_.fg_color = fg;
  if (bg.isValid()) config_.bg_color = bg;
  settings.endGroup();
}

void PreviewTypographyStore::Save() const {
  QSettings settings(ConfigFilePath(), QSettings::IniFormat);
  settings.beginGroup("knuth_plass");
  settings.setValue("kp_enabled", config_.kp_enabled);
  settings.setValue("font_choice", config_.font_choice);
  settings.setValue("font_size_px", config_.font_size_px);
  settings.setValue("auto_width", config_.auto_width);
  settings.setValue("text_width_em", config_.text_width_em);
  settings.setValue("indent_em", config_.indent_em);
  settings.setValue("par_skip_em", config_.par_skip_em);
  settings.remove("line_skip");
  settings.setValue("hyphen_lang", config_.hyphen_lang);
  settings.setValue("runt_penalty", config_.runt_penalty);
  settings.remove("looseness");
  settings.setValue("justify", config_.justify);
  settings.setValue("show_guides", config_.show_guides);
  settings.setValue("theme_preset", config_.theme_preset);
  settings.setValue("fg_color", config_.fg_color.name());
  settings.setValue("bg_color", config_.bg_color.name());
  settings.endGroup();
  settings.sync();
}

void PreviewTypographyStore::UpdateConfig(const PreviewTypographyConfig &cfg) {
  if (config_ == cfg) {
    return;
  }
  config_ = cfg;
  Save();
  emit ConfigChanged(config_);
}

void PreviewTypographyStore::ResetDefaults() {
  UpdateConfig(PreviewTypographyConfig{});
}

QString PreviewTypographyStore::FindFontFile(const QString &file_name) {
  for (const QString &dir : CandidateResourceDirs("fonts")) {
    const QString full = dir + "/" + file_name;
    if (QFileInfo::exists(full)) {
      return full;
    }
  }
  return {};
}

QString PreviewTypographyStore::FindHyphenDictFile(const QString &lang) {
  if (lang.isEmpty()) {
    return {};
  }
  const QString file_name = QString("hyph_%1.dic").arg(lang);
  for (const QString &dir : CandidateResourceDirs("dict")) {
    const QString full = dir + "/" + file_name;
    if (QFileInfo::exists(full)) {
      return full;
    }
  }
  return {};
}

QFont PreviewTypographyStore::ResolveFont(const PreviewTypographyConfig &cfg) const {
  if (!fonts_probed_) {
    fonts_probed_ = true;
    wenyuan_family_ = LoadBundledFontFamily("WenYuanSansSC-Regular.otf");
    lxgw_family_ = LoadBundledFontFamily("LXGWWenKaiLite-Regular.ttf");
  }

  QString family;
  if (cfg.font_choice == "lxgw-wenkai" && !lxgw_family_.isEmpty()) {
    family = lxgw_family_;
  } else if (cfg.font_choice == "wenyuan-sans" && !wenyuan_family_.isEmpty()) {
    family = wenyuan_family_;
  }

  if (family.isEmpty()) {
#ifdef _WIN32
    family = "Microsoft YaHei UI";
#else
    const auto all_families = QFontDatabase().families();
    if (all_families.contains("Noto Sans CJK SC")) {
      family = "Noto Sans CJK SC";
    } else if (!wenyuan_family_.isEmpty()) {
      family = wenyuan_family_;
    } else {
      family = "Sans Serif";
    }
#endif
  }

  QFont f(family);
  f.setPixelSize(std::clamp(cfg.font_size_px, 11, 36));
  f.setStyleStrategy(QFont::PreferAntialias);
  return f;
}

const Hyphenator *PreviewTypographyStore::GetHyphenator(const QString &lang) const {
  if (lang.isEmpty()) {
    return nullptr;
  }
  if (!hyphen_probed_) {
    hyphen_probed_ = true;
    const QString dic = FindHyphenDictFile("en_US");
    if (!dic.isEmpty()) {
      auto h = std::make_unique<Hyphenator>();
      if (h->LoadFromFile(dic)) {
        en_us_hyphen_ = std::move(h);
      }
    }
  }
  return en_us_hyphen_.get();
}

}  // namespace typesetting
}  // namespace gui
}  // namespace soeasy
