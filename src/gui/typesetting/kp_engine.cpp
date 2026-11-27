// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/typesetting/kp_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>

namespace soeasy {
namespace gui {
namespace typesetting {

namespace {

struct Breakpoint {
  int position = -1;
  int line = 0;
  double cum_width = 0.0;
  double cum_stretch = 0.0;
  double cum_shrink = 0.0;
  int64_t total_demerits = 0;
  double ratio = 0.0;
  int64_t demerits = 0;
  int prev_idx = -1;
};

struct CumTotals {
  double width = 0.0;
  double stretch = 0.0;
  double shrink = 0.0;
};

// Totals up to the first Box after a break at `pos` (discards trailing Glue/Penalty,
// stopping before another forced EJECT break). Matches simple.py::cum_after().
CumTotals CumAfter(const std::vector<Item> &items, int pos, const CumTotals &cum) {
  CumTotals after = cum;
  for (int k = pos; k < static_cast<int>(items.size()); ++k) {
    const Item &it = items[k];
    if (it.kind == ItemKind::kBox) {
      break;
    }
    if (it.kind == ItemKind::kGlue) {
      after.width += it.width;
      after.stretch += it.stretch;
      after.shrink += it.shrink;
    } else if (it.penalty <= kEjectPenalty && k > pos) {
      break;
    }
  }
  return after;
}

int ChooseLooseness(const std::vector<int> &active, const std::vector<Breakpoint> &pool, int best_idx,
                    int looseness) {
  const int k = pool[best_idx].line;
  int s = 0;
  for (int idx : active) {
    const Breakpoint &a = pool[idx];
    const int delta = a.line - k;
    if ((looseness <= delta && delta < s) || (s < delta && delta <= looseness)) {
      s = delta;
      best_idx = idx;
    } else if (delta == s && a.total_demerits < pool[best_idx].total_demerits) {
      best_idx = idx;
    }
  }
  return best_idx;
}

}  // namespace

std::optional<std::vector<LineBreakResult>> RunLineBreak(const std::vector<Item> &items,
                                                         const LineBreakOptions &opts) {
  if (items.size() < 2) {
    return std::nullopt;
  }
  const Item &penult = items[items.size() - 2];
  const Item &last = items[items.size() - 1];
  if (penult.kind != ItemKind::kGlue || !std::isinf(penult.stretch) ||
      last.kind != ItemKind::kPenalty || last.penalty != kEjectPenalty) {
    return std::nullopt;
  }
  if (opts.line_widths.empty()) {
    return std::nullopt;
  }

  int j0 = static_cast<int>(opts.line_widths.size()) - 1;
  while (j0 > 0 && opts.line_widths[j0 - 1] == opts.line_widths.back()) {
    --j0;
  }
  if (opts.looseness != 0) {
    j0 = std::numeric_limits<int>::max();
  }

  std::vector<Breakpoint> pool;
  pool.reserve(items.size());
  pool.push_back({-1, 0, 0.0, 0.0, 0.0, 0, 0.0, 0, -1});
  std::vector<int> active = {0};

  CumTotals cum;
  const int N = static_cast<int>(items.size());

  for (int i = 0; i < N; ++i) {
    const Item &item = items[i];
    bool is_break = false;
    int penalty = 0;
    double hyphen_width = 0.0;

    if (item.kind == ItemKind::kGlue) {
      is_break = (i > 0 && items[i - 1].kind == ItemKind::kBox);
    } else if (item.kind == ItemKind::kPenalty && item.penalty < kInfPenalty) {
      is_break = true;
      penalty = item.penalty;
      hyphen_width = item.width;
    }

    if (is_break) {
      const bool forced = (penalty <= kEjectPenalty);
      const CumTotals after = CumAfter(items, i, cum);

      auto ratio_from = [&](const Breakpoint &a) -> double {
        const size_t lw_idx = std::min(static_cast<size_t>(a.line + 1), opts.line_widths.size()) - 1;
        const double line_width = opts.line_widths[lw_idx];
        return ComputeRatio(line_width, (cum.width - a.cum_width) + hyphen_width,
                            cum.stretch - a.cum_stretch, cum.shrink - a.cum_shrink);
      };

      std::vector<int> survivors;
      std::map<int, int> best;
      std::map<int, int> fallback;

      for (int a_idx : active) {
        const Breakpoint &a = pool[a_idx];
        const double r = ratio_from(a);
        if (r >= -1.0 && !forced) {
          survivors.push_back(a_idx);
        }

        const bool feasible = (-1.0 <= r && r <= opts.max_stretch_ratio);
        if (feasible || forced) {
          const int b = feasible ? ComputeBadness(r) : kInfBad;
          const int64_t d = ComputeDemerits(b, penalty, opts.line_penalty, opts.kp81_penalty);
          const int64_t total_d = a.total_demerits + d;
          const int next_line = a.line + 1;
          const int key = std::min(next_line, j0);

          auto &candidates = feasible ? best : fallback;
          auto it = candidates.find(key);
          // "<=": among equals the later predecessor wins, matching TeX and simple.py
          if (it == candidates.end() || total_d <= pool[it->second].total_demerits) {
            pool.push_back({i, next_line, after.width, after.stretch, after.shrink, total_d, r, d, a_idx});
            candidates[key] = static_cast<int>(pool.size()) - 1;
          }
        }
      }

      auto &chosen = !best.empty() ? best : fallback;
      for (const auto &kv : chosen) {
        survivors.push_back(kv.second);
      }

      if (opts.final_pass && survivors.empty() && !active.empty()) {
        // tex.web §854: pick latest position; among different line counts at that position, best total_demerits
        const int last_pos = pool[active.back()].position;
        int best_last = -1;
        for (int k = static_cast<int>(active.size()) - 1; k >= 0; --k) {
          const int a_idx = active[k];
          if (pool[a_idx].position != last_pos) {
            break;
          }
          if (best_last == -1 || pool[a_idx].total_demerits < pool[best_last].total_demerits) {
            best_last = a_idx;
          }
        }
        if (best_last >= 0) {
          const Breakpoint &a = pool[best_last];
          const double r = ratio_from(a);
          pool.push_back({i, a.line + 1, after.width, after.stretch, after.shrink, a.total_demerits, r, 0, best_last});
          survivors.push_back(static_cast<int>(pool.size()) - 1);
        }
      }

      active = std::move(survivors);
      if (active.empty()) {
        return std::nullopt;
      }

      if (forced) {
        cum = CumTotals{};
        const CumTotals reset_after = CumAfter(items, i, cum);
        for (int a_idx : active) {
          pool[a_idx].cum_width = reset_after.width;
          pool[a_idx].cum_stretch = reset_after.stretch;
          pool[a_idx].cum_shrink = reset_after.shrink;
        }
      }
    }

    if (item.kind == ItemKind::kBox) {
      cum.width += item.width;
    } else if (item.kind == ItemKind::kGlue) {
      cum.width += item.width;
      cum.stretch += item.stretch;
      cum.shrink += item.shrink;
    }
  }

  int best_idx = active[0];
  for (int idx : active) {
    if (pool[idx].total_demerits < pool[best_idx].total_demerits) {
      best_idx = idx;
    }
  }
  if (opts.looseness != 0) {
    best_idx = ChooseLooseness(active, pool, best_idx, opts.looseness);
  }

  std::vector<LineBreakResult> breaks;
  int cur = best_idx;
  while (pool[cur].prev_idx != -1) {
    const Breakpoint &bnode = pool[cur];
    const Breakpoint &prev = pool[bnode.prev_idx];
    int start = prev.position + 1;
    while (start < bnode.position && items[start].kind != ItemKind::kBox) {
      ++start;
    }
    breaks.push_back({bnode.line, start, bnode.position, bnode.ratio, bnode.demerits});
    cur = bnode.prev_idx;
  }
  std::reverse(breaks.begin(), breaks.end());
  return breaks;
}

}  // namespace typesetting
}  // namespace gui
}  // namespace soeasy
