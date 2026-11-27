// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/typography_popover.h"

#include <QColorDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QScreen>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "gui/i18n.h"

namespace soeasy {
namespace gui {

TypographyPopover::TypographyPopover(QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
  SetupUi();
  SyncFromStore();
  RetranslateUi();

  connect(&typesetting::PreviewTypographyStore::Instance(),
          &typesetting::PreviewTypographyStore::ConfigChanged, this,
          [this](const typesetting::PreviewTypographyConfig &) { SyncFromStore(); });
}

void TypographyPopover::SetupUi() {
  setAttribute(Qt::WA_StyledBackground, true);
  setMinimumWidth(340);

  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);

  auto *card = new QFrame(this);
  card->setObjectName("kpPopoverCard");
  card->setStyleSheet(
      "#kpPopoverCard {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 8px;"
      "}"
      "QLabel { color: #334155; font-size: 12px; border: none; }"
      "QComboBox {"
      "  font-size: 12px; padding: 4px 28px 4px 8px; border: 1px solid #CBD5E1; border-radius: 4px;"
      "  background: #F8FAFC; color: #0F172A; min-height: 22px;"
      "}"
      "QComboBox:hover { border-color: #94A3B8; }"
      "QComboBox:focus, QComboBox:on { border-color: #0284C7; }"
      "QComboBox::drop-down {"
      "  subcontrol-origin: padding; subcontrol-position: top right; width: 24px; border: none; background: transparent;"
      "}"
      "QComboBox::down-arrow {"
      "  image: url(:/icons/chevron_down.png); width: 11px; height: 11px;"
      "}"
      "QSpinBox, QDoubleSpinBox {"
      "  font-size: 12px; padding: 3px 24px 3px 8px; border: 1px solid #CBD5E1; border-radius: 4px;"
      "  background: #F8FAFC; color: #0F172A; min-height: 22px;"
      "}"
      "QSpinBox:hover, QDoubleSpinBox:hover { border-color: #94A3B8; }"
      "QSpinBox:focus, QDoubleSpinBox:focus { border-color: #0284C7; }"
      "QSpinBox:disabled, QDoubleSpinBox:disabled { background: #F1F5F9; color: #94A3B8; border-color: #E2E8F0; }"
      "QSpinBox::up-button, QSpinBox::down-button,"
      "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {"
      "  width: 20px; border: none; background: #F1F5F9;"
      "}"
      "QSpinBox::up-button, QDoubleSpinBox::up-button {"
      "  subcontrol-origin: border; subcontrol-position: top right;"
      "  border-top-right-radius: 3px; border-left: 1px solid #E2E8F0;"
      "}"
      "QSpinBox::down-button, QDoubleSpinBox::down-button {"
      "  subcontrol-origin: border; subcontrol-position: bottom right;"
      "  border-bottom-right-radius: 3px; border-left: 1px solid #E2E8F0;"
      "}"
      "QSpinBox::up-button:hover, QSpinBox::down-button:hover,"
      "QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: #E2E8F0; }"
      "QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {"
      "  image: url(:/icons/chevron_up.png); width: 10px; height: 10px;"
      "}"
      "QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {"
      "  image: url(:/icons/chevron_down.png); width: 10px; height: 10px;"
      "}"
      "QCheckBox { font-size: 12px; color: #334155; spacing: 6px; }");

  auto *card_layout = new QVBoxLayout(card);
  card_layout->setContentsMargins(14, 12, 14, 12);
  card_layout->setSpacing(10);

  auto *header_row = new QHBoxLayout();
  lbl_title_ = new QLabel(card);
  lbl_title_->setStyleSheet("font-size: 13px; font-weight: bold; color: #0F172A;");
  btn_reset_ = new QPushButton(card);
  btn_reset_->setCursor(Qt::PointingHandCursor);
  btn_reset_->setStyleSheet(
      "QPushButton { font-size: 11px; padding: 3px 8px; border: 1px solid #CBD5E1; border-radius: 4px; "
      "background: #F8FAFC; color: #475569; }"
      "QPushButton:hover { background: #F1F5F9; color: #0F172A; }");
  header_row->addWidget(lbl_title_);
  header_row->addStretch();
  header_row->addWidget(btn_reset_);
  card_layout->addLayout(header_row);

  auto *form = new QFormLayout();
  form->setSpacing(7);
  form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

  lbl_font_ = new QLabel(card);
  combo_font_ = new QComboBox(card);
  combo_font_->addItem("LXGW WenKai Lite", "lxgw-wenkai");
  combo_font_->addItem("WenYuan Sans SC", "wenyuan-sans");
  combo_font_->addItem("System Default", "system");
  form->addRow(lbl_font_, combo_font_);

  lbl_size_ = new QLabel(card);
  spin_size_ = new QSpinBox(card);
  spin_size_->setRange(11, 36);
  form->addRow(lbl_size_, spin_size_);

  lbl_width_ = new QLabel(card);
  auto *width_row = new QHBoxLayout();
  width_row->setSpacing(6);
  chk_auto_width_ = new QCheckBox(card);
  spin_width_ = new QDoubleSpinBox(card);
  spin_width_->setRange(8.0, 80.0);
  spin_width_->setSingleStep(0.5);
  spin_width_->setDecimals(1);
  width_row->addWidget(chk_auto_width_);
  width_row->addWidget(spin_width_, 1);
  form->addRow(lbl_width_, width_row);

  lbl_indent_ = new QLabel(card);
  spin_indent_ = new QDoubleSpinBox(card);
  spin_indent_->setRange(0.0, 10.0);
  spin_indent_->setSingleStep(0.5);
  spin_indent_->setDecimals(1);
  form->addRow(lbl_indent_, spin_indent_);

  lbl_parskip_ = new QLabel(card);
  spin_parskip_ = new QDoubleSpinBox(card);
  spin_parskip_->setRange(0.0, 5.0);
  spin_parskip_->setSingleStep(0.1);
  spin_parskip_->setDecimals(1);
  form->addRow(lbl_parskip_, spin_parskip_);

  lbl_hyphen_ = new QLabel(card);
  combo_hyphen_ = new QComboBox(card);
  combo_hyphen_->addItem("en_US", "en_US");
  combo_hyphen_->addItem("Off", "");
  form->addRow(lbl_hyphen_, combo_hyphen_);

  lbl_runt_ = new QLabel(card);
  spin_runt_ = new QSpinBox(card);
  spin_runt_->setRange(0, 1000);
  spin_runt_->setSingleStep(50);
  form->addRow(lbl_runt_, spin_runt_);

  lbl_theme_ = new QLabel(card);
  combo_theme_ = new QComboBox(card);
  combo_theme_->addItem("Light", "light");
  combo_theme_->addItem("Sepia", "sepia");
  combo_theme_->addItem("Dark", "dark");
  combo_theme_->addItem("Custom", "custom");
  form->addRow(lbl_theme_, combo_theme_);

  lbl_colors_ = new QLabel(card);
  auto *color_row = new QHBoxLayout();
  color_row->setSpacing(6);
  btn_fg_color_ = new QPushButton(card);
  btn_bg_color_ = new QPushButton(card);
  color_row->addWidget(btn_fg_color_, 1);
  color_row->addWidget(btn_bg_color_, 1);
  form->addRow(lbl_colors_, color_row);

  card_layout->addLayout(form);

  auto *flags_row = new QHBoxLayout();
  flags_row->setSpacing(14);
  chk_justify_ = new QCheckBox(card);
  chk_guides_ = new QCheckBox(card);
  flags_row->addWidget(chk_justify_);
  flags_row->addWidget(chk_guides_);
  flags_row->addStretch();
  card_layout->addLayout(flags_row);

  outer->addWidget(card);

  auto wire_change = [this]() { PushUiToStore(); };
  connect(combo_font_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, wire_change);
  connect(spin_size_, QOverload<int>::of(&QSpinBox::valueChanged), this, wire_change);
  connect(chk_auto_width_, &QCheckBox::toggled, this, [this](bool auto_fit) {
    spin_width_->setEnabled(!auto_fit);
    PushUiToStore();
  });
  connect(spin_width_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, wire_change);
  connect(spin_indent_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, wire_change);
  connect(spin_parskip_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, wire_change);
  connect(combo_hyphen_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, wire_change);
  connect(spin_runt_, QOverload<int>::of(&QSpinBox::valueChanged), this, wire_change);
  connect(chk_justify_, &QCheckBox::toggled, this, wire_change);
  connect(chk_guides_, &QCheckBox::toggled, this, wire_change);

  connect(combo_theme_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
    auto cfg = typesetting::PreviewTypographyStore::Instance().config();
    const QString preset = combo_theme_->currentData().toString();
    typesetting::PreviewTypographyConfig::ApplyThemePreset(preset, &cfg);
    typesetting::PreviewTypographyStore::Instance().UpdateConfig(cfg);
  });

  connect(btn_fg_color_, &QPushButton::clicked, this, [this]() {
    auto cfg = typesetting::PreviewTypographyStore::Instance().config();
    const QColor chosen = QColorDialog::getColor(cfg.fg_color, this, Tr("kp_pick_fg_color"));
    if (chosen.isValid()) {
      cfg.theme_preset = "custom";
      cfg.fg_color = chosen;
      typesetting::PreviewTypographyStore::Instance().UpdateConfig(cfg);
    }
  });

  connect(btn_bg_color_, &QPushButton::clicked, this, [this]() {
    auto cfg = typesetting::PreviewTypographyStore::Instance().config();
    const QColor chosen = QColorDialog::getColor(cfg.bg_color, this, Tr("kp_pick_bg_color"));
    if (chosen.isValid()) {
      cfg.theme_preset = "custom";
      cfg.bg_color = chosen;
      typesetting::PreviewTypographyStore::Instance().UpdateConfig(cfg);
    }
  });

  connect(btn_reset_, &QPushButton::clicked, this, []() {
    typesetting::PreviewTypographyStore::Instance().ResetDefaults();
  });
}

void TypographyPopover::UpdateColorButtons(const typesetting::PreviewTypographyConfig &cfg) {
  auto make_style = [](const QColor &c) {
    const QColor text_c = (c.lightness() < 128) ? QColor("#F8FAFC") : QColor("#0F172A");
    return QString("QPushButton { font-size: 11px; padding: 3px 6px; border: 1px solid #CBD5E1; "
                   "border-radius: 4px; background-color: %1; color: %2; }")
        .arg(c.name(), text_c.name());
  };
  btn_fg_color_->setStyleSheet(make_style(cfg.fg_color));
  btn_bg_color_->setStyleSheet(make_style(cfg.bg_color));
}

void TypographyPopover::SyncFromStore() {
  const auto &cfg = typesetting::PreviewTypographyStore::Instance().config();
  const QSignalBlocker b1(combo_font_), b2(spin_size_), b3(chk_auto_width_), b4(spin_width_),
      b5(spin_indent_), b6(spin_parskip_), b7(combo_hyphen_), b8(spin_runt_),
      b9(combo_theme_), b10(chk_justify_), b11(chk_guides_);

  int fi = combo_font_->findData(cfg.font_choice);
  if (fi >= 0) combo_font_->setCurrentIndex(fi);
  spin_size_->setValue(cfg.font_size_px);
  chk_auto_width_->setChecked(cfg.auto_width);
  spin_width_->setEnabled(!cfg.auto_width);
  spin_width_->setValue(cfg.text_width_em);
  spin_indent_->setValue(cfg.indent_em);
  spin_parskip_->setValue(cfg.par_skip_em);

  int hi = combo_hyphen_->findData(cfg.hyphen_lang);
  if (hi >= 0) combo_hyphen_->setCurrentIndex(hi);
  spin_runt_->setValue(cfg.runt_penalty);

  int ti = combo_theme_->findData(cfg.theme_preset);
  if (ti >= 0) combo_theme_->setCurrentIndex(ti);
  chk_justify_->setChecked(cfg.justify);
  chk_guides_->setChecked(cfg.show_guides);
  UpdateColorButtons(cfg);
}

void TypographyPopover::PushUiToStore() {
  auto cfg = typesetting::PreviewTypographyStore::Instance().config();
  cfg.font_choice = combo_font_->currentData().toString();
  cfg.font_size_px = spin_size_->value();
  cfg.auto_width = chk_auto_width_->isChecked();
  cfg.text_width_em = spin_width_->value();
  cfg.indent_em = spin_indent_->value();
  cfg.par_skip_em = spin_parskip_->value();
  cfg.line_skip = 1.4;
  cfg.hyphen_lang = combo_hyphen_->currentData().toString();
  cfg.runt_penalty = spin_runt_->value();
  cfg.looseness = 0;
  cfg.justify = chk_justify_->isChecked();
  cfg.show_guides = chk_guides_->isChecked();
  typesetting::PreviewTypographyStore::Instance().UpdateConfig(cfg);
}

void TypographyPopover::RetranslateUi() {
  lbl_title_->setText(Tr("kp_settings_title"));
  btn_reset_->setText(Tr("kp_reset_defaults"));
  lbl_font_->setText(Tr("kp_label_font"));
  lbl_size_->setText(Tr("kp_label_size"));
  lbl_width_->setText(Tr("kp_label_width"));
  chk_auto_width_->setText(Tr("kp_auto_width"));
  lbl_indent_->setText(Tr("kp_label_indent"));
  lbl_parskip_->setText(Tr("kp_label_parskip"));
  lbl_hyphen_->setText(Tr("kp_label_hyphen"));
  lbl_runt_->setText(Tr("kp_label_runt"));
  lbl_theme_->setText(Tr("kp_label_theme"));
  lbl_colors_->setText(Tr("kp_label_colors"));
  chk_justify_->setText(Tr("kp_justify"));
  chk_guides_->setText(Tr("kp_guides"));

  combo_font_->setItemText(0, Tr("kp_font_lxgw"));
  combo_font_->setItemText(1, Tr("kp_font_wenyuan"));
  combo_font_->setItemText(2, Tr("kp_font_system"));
  combo_hyphen_->setItemText(0, Tr("kp_hyphen_en"));
  combo_hyphen_->setItemText(1, Tr("kp_hyphen_off"));
  combo_theme_->setItemText(0, Tr("kp_theme_light"));
  combo_theme_->setItemText(1, Tr("kp_theme_sepia"));
  combo_theme_->setItemText(2, Tr("kp_theme_dark"));
  combo_theme_->setItemText(3, Tr("kp_theme_custom"));
  btn_fg_color_->setText(Tr("kp_fg_color"));
  btn_bg_color_->setText(Tr("kp_bg_color"));
}

void TypographyPopover::ShowBelow(QWidget *anchor) {
  if (!anchor) return;
  RetranslateUi();
  SyncFromStore();
  adjustSize();
  const QPoint bottom_right = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height() + 4));
  QPoint target(bottom_right.x() - width(), bottom_right.y());
  if (QScreen *scr = QGuiApplication::screenAt(bottom_right)) {
    const QRect avail = scr->availableGeometry();
    target.setX(std::clamp(target.x(), avail.left() + 8, avail.right() - width() - 8));
    target.setY(std::clamp(target.y(), avail.top() + 8, avail.bottom() - height() - 8));
  }
  move(target);
  show();
  raise();
}

}  // namespace gui
}  // namespace soeasy
