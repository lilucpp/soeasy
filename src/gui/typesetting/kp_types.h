// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_TYPESETTING_KP_TYPES_H_
#define SOEASY_SRC_GUI_TYPESETTING_KP_TYPES_H_

#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace soeasy {
namespace gui {
namespace typesetting {

constexpr int kInfBad = 10000;
constexpr int kInfPenalty = 10000;
constexpr int kEjectPenalty = -10000;
constexpr int kHyphenPenalty = 50;
constexpr int kDefaultLinePenalty = 10;

enum class ItemKind {
  kBox,
  kGlue,
  kPenalty,
};

/**
 * @brief Paragraph element in the Knuth-Plass Box/Glue/Penalty model.
 *
 * Ports chenshuo/typeset/linebreak/common.py and adds optional visual/source
 * metadata (`draw_dx`, `src_start`, `src_end`) for Qt5 rendering, selection, and
 * keyword highlighting without duplicating text buffers.
 */
struct Item {
  ItemKind kind = ItemKind::kBox;
  double width = 0.0;
  double stretch = 0.0;
  double shrink = 0.0;
  int penalty = 0;
  bool is_hyphen = false;
  QString text;

  // Rendering offset applied relative to box x (replaces OpenType halt in Qt5)
  double draw_dx = 0.0;
  // UTF-16 code-unit span inside the source paragraph (`src_end > src_start` for text boxes)
  int src_start = -1;
  int src_end = -1;

  static Item Box(double w, const QString &t = QString(), double dx = 0.0, int s_start = -1, int s_end = -1) {
    Item it;
    it.kind = ItemKind::kBox;
    it.width = w;
    it.text = t;
    it.draw_dx = dx;
    it.src_start = s_start;
    it.src_end = s_end;
    return it;
  }

  static Item Glue(double w, double y, double z, const QString &t = " ") {
    Item it;
    it.kind = ItemKind::kGlue;
    it.width = w;
    it.stretch = y;
    it.shrink = z;
    it.text = t;
    return it;
  }

  static Item ZeroGlue() { return Glue(0.0, 0.0, 0.0, QString()); }

  static Item InfGlue() { return Glue(0.0, std::numeric_limits<double>::infinity(), 0.0, "$$"); }

  static Item Penalty(int value, double w = 0.0, const QString &t = QString(), bool hyphen = false) {
    Item it;
    it.kind = ItemKind::kPenalty;
    it.penalty = value;
    it.width = w;
    it.text = t;
    it.is_hyphen = hyphen;
    return it;
  }

  static Item NoBreak() { return Penalty(kInfPenalty); }
  static Item Eject() { return Penalty(kEjectPenalty); }

  double GetGlueWidth(double ratio) const {
    if (ratio == 0.0 || !std::isfinite(ratio)) {
      return width;
    }
    const double amount = (ratio > 0.0) ? stretch : shrink;
    return width + ratio * amount;
  }
};

struct LineBreakResult {
  int line = 0;   // 1-based line index
  int start = 0;  // index of first Box on this line
  int end = 0;    // index of break item (exclusive of line content)
  double ratio = 0.0;
  int64_t demerits = 0;
};

inline double ComputeRatio(double line_width, double total_width, double total_stretch, double total_shrink) {
  if (total_width == line_width) {
    return 0.0;
  }
  if (total_width < line_width) {
    if (std::isinf(total_stretch)) {
      return 0.0;
    }
    if (total_stretch > 0.0) {
      return (line_width - total_width) / total_stretch;
    }
    return std::numeric_limits<double>::infinity();
  }
  if (total_shrink > 0.0) {
    return (line_width - total_width) / total_shrink;
  }
  return -std::numeric_limits<double>::infinity();
}

inline int ComputeBadness(double ratio) {
  if (!std::isfinite(ratio) || ratio < -1.0) {
    return kInfBad;
  }
  return std::min(kInfBad, static_cast<int>(100.0 * std::pow(std::abs(ratio), 3) + 0.5));
}

inline int64_t ComputeDemerits(int badness, int penalty, int line_penalty = kDefaultLinePenalty,
                               bool kp81_penalty = false) {
  int64_t lb = static_cast<int64_t>(line_penalty) + static_cast<int64_t>(badness);
  if (penalty >= 0) {
    if (kp81_penalty) {
      int64_t lbp = lb + static_cast<int64_t>(penalty);
      return lbp * lbp;
    }
    return lb * lb + static_cast<int64_t>(penalty) * static_cast<int64_t>(penalty);
  }
  int64_t d = lb * lb;
  if (penalty > kEjectPenalty) {
    d -= static_cast<int64_t>(penalty) * static_cast<int64_t>(penalty);
  }
  return d;
}

}  // namespace typesetting
}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_TYPESETTING_KP_TYPES_H_
