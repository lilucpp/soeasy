// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_TYPESETTING_KP_ENGINE_H_
#define SOEASY_SRC_GUI_TYPESETTING_KP_ENGINE_H_

#include <optional>
#include <vector>

#include "gui/typesetting/kp_types.h"

namespace soeasy {
namespace gui {
namespace typesetting {

struct LineBreakOptions {
  // Single line width or per-line widths (last element repeats for subsequent lines).
  std::vector<double> line_widths;
  double max_stretch_ratio = 1.0;
  int looseness = 0;
  bool final_pass = false;
  int line_penalty = kDefaultLinePenalty;
  bool kp81_penalty = false;
};

/**
 * @brief Knuth-Plass dynamic programming line-breaker faithfully ported from
 * chenshuo/typeset/linebreak/simple.py.
 *
 * Returns nullopt if no feasible break sequence exists under `max_stretch_ratio`
 * (when `final_pass == false`), or always returns a valid break sequence when
 * `final_pass == true` (assuming valid paragraph terminator Glue.INF + EJECT).
 */
std::optional<std::vector<LineBreakResult>> RunLineBreak(const std::vector<Item> &items,
                                                         const LineBreakOptions &opts);

}  // namespace typesetting
}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_TYPESETTING_KP_ENGINE_H_
