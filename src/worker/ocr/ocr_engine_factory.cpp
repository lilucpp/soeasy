// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/ocr/ocr_engine_factory.h"

#include <filesystem>
#include <vector>

#include "common/logger.h"
#include "worker/ocr/ppocr_v6_engine.h"

namespace soeasy {
namespace worker {
namespace ocr {

std::unique_ptr<IOcrEngine> OcrEngineFactory::CreateEngine(OcrEngineType type) {
  if (type == OcrEngineType::kPPOCRv6) {
    return std::make_unique<PPOCRv6Engine>();
  }
  return nullptr;
}

std::unique_ptr<IOcrEngine> OcrEngineFactory::CreateDefaultEngine(
    const std::string& models_base_dir, OcrMode mode) {
  std::vector<std::string> candidate_dirs;
  if (!models_base_dir.empty()) {
    candidate_dirs.push_back(models_base_dir);
  }
  candidate_dirs.push_back("models/ppocr_v6");
  candidate_dirs.push_back("./models/ppocr_v6");
  candidate_dirs.push_back("../models/ppocr_v6");
  candidate_dirs.push_back("/home/peanut/project/soeasy/models/ppocr_v6");
  candidate_dirs.push_back("/usr/share/soeasy/models/ppocr_v6");

  std::string found_dir;
  for (const auto& dir : candidate_dirs) {
    if (std::filesystem::exists(dir + "/PP-OCRv6_tiny_det_onnx/inference.onnx")) {
      found_dir = dir;
      break;
    }
  }

  if (found_dir.empty()) {
    LOG_ERROR("[OcrEngineFactory] Could not find PP-OCRv6 models in candidate directories");
    return nullptr;
  }

  auto engine = CreateEngine(OcrEngineType::kPPOCRv6);
  if (!engine) return nullptr;

  OcrConfig config;
  config.det_model_path = found_dir + "/PP-OCRv6_tiny_det_onnx/inference.onnx";
  config.rec_model_path = found_dir + "/PP-OCRv6_tiny_rec_onnx/inference.onnx";
  config.dict_path = found_dir + "/PP-OCRv6_tiny_rec_onnx/chars.txt";
  config.mode = mode;

  if (!engine->Initialize(config)) {
    LOG_ERROR("[OcrEngineFactory] Failed to initialize PP-OCRv6 engine from '{}'", found_dir);
    return nullptr;
  }

  return engine;
}

}  // namespace ocr
}  // namespace worker
}  // namespace soeasy
