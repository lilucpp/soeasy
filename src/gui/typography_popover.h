// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_TYPOGRAPHY_POPOVER_H_
#define SOEASY_SRC_GUI_TYPOGRAPHY_POPOVER_H_

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QWidget>

#include "gui/typesetting/preview_typography.h"

namespace soeasy {
namespace gui {

/**
 * @brief Floating settings popover exposing Knuth-Plass reader options
 * (font, size, line width/auto-fit, indent, parskip, line-height, English
 * hyphenation, runt penalty, looseness, colors, justify, and guides).
 */
class TypographyPopover : public QWidget {
  Q_OBJECT

 public:
  explicit TypographyPopover(QWidget *parent = nullptr);
  ~TypographyPopover() override = default;

  void SyncFromStore();
  void RetranslateUi();
  void ShowBelow(QWidget *anchor);

 private:
  void SetupUi();
  void PushUiToStore();
  void UpdateColorButtons(const typesetting::PreviewTypographyConfig &cfg);

  QLabel *lbl_title_ = nullptr;
  QLabel *lbl_font_ = nullptr;
  QLabel *lbl_size_ = nullptr;
  QLabel *lbl_width_ = nullptr;
  QLabel *lbl_indent_ = nullptr;
  QLabel *lbl_parskip_ = nullptr;
  QLabel *lbl_hyphen_ = nullptr;
  QLabel *lbl_runt_ = nullptr;
  QLabel *lbl_theme_ = nullptr;
  QLabel *lbl_colors_ = nullptr;

  QComboBox *combo_font_ = nullptr;
  QSpinBox *spin_size_ = nullptr;
  QCheckBox *chk_auto_width_ = nullptr;
  QDoubleSpinBox *spin_width_ = nullptr;
  QDoubleSpinBox *spin_indent_ = nullptr;
  QDoubleSpinBox *spin_parskip_ = nullptr;
  QComboBox *combo_hyphen_ = nullptr;
  QSpinBox *spin_runt_ = nullptr;
  QComboBox *combo_theme_ = nullptr;
  QPushButton *btn_fg_color_ = nullptr;
  QPushButton *btn_bg_color_ = nullptr;
  QCheckBox *chk_justify_ = nullptr;
  QCheckBox *chk_guides_ = nullptr;
  QPushButton *btn_reset_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_TYPOGRAPHY_POPOVER_H_
