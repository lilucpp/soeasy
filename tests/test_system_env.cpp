// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "common/system_env.h"

#include <cassert>
#include <iostream>

#if defined(__linux__)
#include <sys/resource.h>
#elif defined(_WIN32)
#include <stdio.h>
#endif

#include "common/logger.h"

int main() {
  std::cout << "[TestSystemEnv] Starting System Environment optimization test..." << std::endl;

  // 1. Test Process File Limits Optimization
  bool fd_res = soeasy::common::SystemEnv::OptimizeProcessFileLimits();
  (void)fd_res;
  assert(fd_res);

#if defined(__linux__)
  struct rlimit rl;
  int ret = getrlimit(RLIMIT_NOFILE, &rl);
  (void)ret;
  assert(ret == 0);
  uint64_t target = (rl.rlim_max == RLIM_INFINITY) ? 1048576ULL : static_cast<uint64_t>(rl.rlim_max);
  std::cout << "[TestSystemEnv] Linux RLIMIT_NOFILE: cur=" << rl.rlim_cur
            << ", max=" << rl.rlim_max << ", target=" << target << std::endl;
  assert(static_cast<uint64_t>(rl.rlim_cur) == target);
#elif defined(_WIN32)
  int cur_max = _getmaxstdio();
  std::cout << "[TestSystemEnv] Windows _getmaxstdio: " << cur_max << std::endl;
  assert(cur_max >= 512);
#endif

  // 2. Test Inotify Limits Inspection & Optimization
  auto inotify_info = soeasy::common::SystemEnv::CheckAndOptimizeInotifyWatches(524288);
  std::cout << "[TestSystemEnv] Inotify watches=" << inotify_info.current_watches
            << ", write_perm=" << (inotify_info.has_write_permission ? "true" : "false")
            << ", updated=" << (inotify_info.updated ? "true" : "false")
            << ", sufficient=" << (inotify_info.is_sufficient ? "true" : "false") << std::endl;

#if defined(__linux__)
  assert(inotify_info.current_watches > 0);
  std::string guide = soeasy::common::SystemEnv::GetInotifyTuningGuide(inotify_info.current_watches);
  assert(guide.find("fs.inotify.max_user_watches=524288") != std::string::npos);
  std::cout << "[TestSystemEnv] Inotify Tuning Guide Output:\n" << guide << std::endl;
#endif

  std::cout << "[TestSystemEnv] ALL TESTS PASSED SUCCESSFULLY!" << std::endl;
  return 0;
}
