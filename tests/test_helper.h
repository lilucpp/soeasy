// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_TESTS_TEST_HELPER_H_
#define SOEASY_TESTS_TEST_HELPER_H_

#include <filesystem>
#include <string>
#include <vector>

namespace soeasy {
namespace test {

inline std::filesystem::path GetExecutableDir() {
  std::error_code ec;
  auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec && !p.empty()) {
    return p.parent_path();
  }
  return std::filesystem::current_path();
}

inline std::filesystem::path GetProjectRootDir() {
  auto exe_dir = GetExecutableDir();
  // Typical CMake build layout: <project_root>/build/<test_executable>
  if (std::filesystem::exists(exe_dir.parent_path() / "CMakeLists.txt")) {
    return exe_dir.parent_path();
  }
  if (std::filesystem::exists(exe_dir / "CMakeLists.txt")) {
    return exe_dir;
  }
  return std::filesystem::current_path();
}

inline std::string FindDictDir() {
  auto root = GetProjectRootDir();
  auto exe_dir = GetExecutableDir();
  std::vector<std::filesystem::path> candidates = {
      root / "third_party/cppjieba/dict",
      exe_dir / "dict",
      exe_dir / "../dict",
      "/usr/local/share/soeasy/dict",
      "/usr/share/soeasy/dict"
  };
  for (const auto& c : candidates) {
    if (std::filesystem::exists(c / "jieba.dict.utf8")) {
      return c.string();
    }
  }
  return (root / "third_party/cppjieba/dict").string();
}

inline std::string FindWorkerBin() {
  auto root = GetProjectRootDir();
  auto exe_dir = GetExecutableDir();
  std::vector<std::filesystem::path> candidates = {
      exe_dir / "soeasy-worker",
      root / "build/soeasy-worker",
      exe_dir / "bin/soeasy-worker",
      exe_dir / "../bin/soeasy-worker"
  };
  for (const auto& c : candidates) {
    if (std::filesystem::exists(c)) {
      return c.string();
    }
  }
  return (exe_dir / "soeasy-worker").string();
}

inline std::string FindModelsDir() {
  auto root = GetProjectRootDir();
  auto exe_dir = GetExecutableDir();
  std::vector<std::filesystem::path> candidates = {
      root / "models/ppocr_v6",
      exe_dir / "models/ppocr_v6",
      exe_dir / "../models/ppocr_v6",
      "/usr/local/share/soeasy/models/ppocr_v6",
      "/usr/share/soeasy/models/ppocr_v6"
  };
  for (const auto& c : candidates) {
    if (std::filesystem::exists(c / "PP-OCRv6_tiny_det_onnx/inference.onnx")) {
      return c.string();
    }
  }
  return (root / "models/ppocr_v6").string();
}

inline std::string FindTestDatasetDir() {
  auto root = GetProjectRootDir();
  std::vector<std::filesystem::path> candidates = {
      root.parent_path() / "test_dataset",
      root / "test_dataset",
      root / "../test_dataset"
  };
  for (const auto& c : candidates) {
    if (std::filesystem::exists(c)) {
      return c.string();
    }
  }
  return (root.parent_path() / "test_dataset").string();
}

}  // namespace test
}  // namespace soeasy

#endif  // SOEASY_TESTS_TEST_HELPER_H_
