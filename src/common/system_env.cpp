// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "common/system_env.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <stdio.h>
#include <windows.h>
#endif

#include "common/logger.h"

namespace soeasy {
namespace common {

bool SystemEnv::OptimizeProcessFileLimits() {
#if defined(__linux__)
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
    LOG_WARN("[SystemEnv] Failed to getrlimit(RLIMIT_NOFILE): {}", std::strerror(errno));
    return false;
  }

  uint64_t target = (rl.rlim_max == RLIM_INFINITY) ? 1048576ULL : static_cast<uint64_t>(rl.rlim_max);
  if (static_cast<uint64_t>(rl.rlim_cur) < target) {
    uint64_t old_cur = rl.rlim_cur;
    rl.rlim_cur = target;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
      LOG_WARN("[SystemEnv] Failed to setrlimit(RLIMIT_NOFILE) to {}: {}", target, std::strerror(errno));
      return false;
    }
    LOG_INFO("[SystemEnv] Optimized process file descriptor limit: {} -> {}", old_cur, target);
    return true;
  }
  LOG_DEBUG("[SystemEnv] Process file descriptor limit already at maximum: {}", rl.rlim_cur);
  return true;
#elif defined(_WIN32)
  int cur_max = _getmaxstdio();
  if (cur_max < 2048) {
    int new_max = _setmaxstdio(2048);
    LOG_INFO("[SystemEnv] Optimized Windows CRT max stdio file handle limit: {} -> {}", cur_max, new_max);
    return true;
  }
  LOG_DEBUG("[SystemEnv] Windows CRT max stdio already at maximum: {}", cur_max);
  return true;
#else
  return true;
#endif
}

InotifyLimitInfo SystemEnv::CheckAndOptimizeInotifyWatches(int target_watches) {
  InotifyLimitInfo info;
#if defined(__linux__)
  const char *proc_path = "/proc/sys/fs/inotify/max_user_watches";
  int current = -1;
  {
    std::ifstream ifs(proc_path);
    if (ifs.is_open()) {
      ifs >> current;
    }
  }

  info.current_watches = current;
  info.has_write_permission = (geteuid() == 0) || (access(proc_path, W_OK) == 0);

  if (info.has_write_permission) {
    if (current > 0 && current < target_watches) {
      std::ofstream ofs(proc_path);
      if (ofs.is_open()) {
        ofs << target_watches << std::endl;
        if (ofs.good()) {
          info.updated = true;
          LOG_INFO("[SystemEnv] Permission granted: inotify max_user_watches updated from {} to {}",
                   current, target_watches);
          info.current_watches = target_watches;
        } else {
          LOG_WARN("[SystemEnv] Write to {} failed: {}", proc_path, std::strerror(errno));
        }
      } else {
        LOG_WARN("[SystemEnv] Unable to open {} for writing: {}", proc_path, std::strerror(errno));
      }
    } else if (current >= target_watches) {
      LOG_INFO("[SystemEnv] Inotify max_user_watches ({}) is already sufficient (>= target {}).",
               current, target_watches);
    }
  } else {
    LOG_INFO("[SystemEnv] Current inotify max_user_watches: {} (Read-only mode, no root/write permission)",
             info.current_watches);
  }

  info.is_sufficient = (info.current_watches >= 65536);
  if (!info.is_sufficient && info.current_watches > 0) {
    LOG_WARN("[SystemEnv] Inotify max_user_watches ({}) is below recommended 65536.{}",
             info.current_watches, GetInotifyTuningGuide(info.current_watches));
  }
#else
  info.current_watches = target_watches;
  info.has_write_permission = true;
  info.is_sufficient = true;
#endif
  return info;
}

std::string SystemEnv::GetInotifyTuningGuide(int current_watches) {
  std::string guide;
  guide += "\n================================================================================\n";
  guide += "[System Notice] Inotify directory watch quota is low or exhausted!\n";
  if (current_watches > 0) {
    guide += "Current limit: " + std::to_string(current_watches) + " watches.\n";
  }
  guide += "To monitor large project directories without missing change events,\n";
  guide += "please run the following command in your terminal to raise the kernel limit:\n\n";
  guide += "  echo fs.inotify.max_user_watches=524288 | sudo tee -a /etc/sysctl.d/99-inotify.conf && sudo sysctl -p /etc/sysctl.d/99-inotify.conf\n\n";
  guide += "================================================================================";
  return guide;
}

}  // namespace common
}  // namespace soeasy
