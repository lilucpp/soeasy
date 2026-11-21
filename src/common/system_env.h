// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#pragma once

#include <cstdint>
#include <string>

namespace soeasy {
namespace common {

struct InotifyLimitInfo {
  int current_watches{0};
  bool has_write_permission{false};
  bool updated{false};
  bool is_sufficient{true};  // True if current_watches >= recommended threshold (65536)
};

class SystemEnv {
 public:
  // Optimize process file descriptor limits to maximum allowed by OS.
  // Linux: lifts rlim_cur up to rlim_max (no root required).
  // Windows: adjusts _setmaxstdio to 2048 (no admin required).
  static bool OptimizeProcessFileLimits();

  // Inspect and optimize Linux inotify watches limit.
  // If permission is available (e.g., root/privileged container), updates it directly.
  // If permission is absent, prints current limit and flags if insufficient.
  static InotifyLimitInfo CheckAndOptimizeInotifyWatches(int target_watches = 524288);

  // Generates user-friendly tuning guide with copy-paste shell command.
  static std::string GetInotifyTuningGuide(int current_watches);

  // Cap glibc malloc arenas early before spawning runtime threads to reduce
  // multi-arena RSS fragmentation (no-op on non-glibc platforms).
  static void ConfigureAllocator(int max_arenas = 2) noexcept;

  // Release free heap pages back to the OS on glibc platforms (no-op elsewhere).
  static void TrimHeap() noexcept;
};

}  // namespace common
}  // namespace soeasy
