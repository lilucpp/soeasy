// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_OCR_OCR_TYPES_H_
#define SOEASY_SRC_WORKER_OCR_OCR_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

namespace soeasy {
namespace worker {
namespace ocr {

enum class OcrMode {
  kNormal = 0,          // General mode: <= 1 CPU core/thread, minimal resource footprint
  kHighPerformance = 1  // High performance: multi-threaded parallel inference
};

enum class OcrEngineType {
  kPPOCRv6 = 0,
  kCustom = 1
};

struct OcrPoint {
  float x = 0.0f;
  float y = 0.0f;
};

struct OcrBox {
  std::vector<OcrPoint> points;  // 4 corners: TL, TR, BR, BL
  float score = 0.0f;
};

struct OcrLine {
  std::string text;
  float confidence = 0.0f;
  OcrBox box;
};

struct OcrResult {
  bool success = false;
  std::vector<OcrLine> lines;
  std::string full_text;
  double det_ms = 0.0;
  double rec_ms = 0.0;
  double total_ms = 0.0;
};

struct OcrConfig {
  std::string det_model_path;
  std::string rec_model_path;
  std::string dict_path;
  OcrMode mode = OcrMode::kNormal;
  int limit_side_len = 736;
  int max_side_limit = 4000;
  float det_db_thresh = 0.3f;
  float det_box_thresh = 0.6f;
  float unclip_ratio = 1.5f;
};

}  // namespace ocr
}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_OCR_OCR_TYPES_H_
