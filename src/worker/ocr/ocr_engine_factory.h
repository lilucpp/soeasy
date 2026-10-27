// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_OCR_OCR_ENGINE_FACTORY_H_
#define SOEASY_SRC_WORKER_OCR_OCR_ENGINE_FACTORY_H_

#include <memory>
#include <string>

#include "worker/ocr/i_ocr_engine.h"

namespace soeasy {
namespace worker {
namespace ocr {

class OcrEngineFactory {
 public:
  /**
   * @brief Create an OCR engine instance for the requested engine type.
   */
  static std::unique_ptr<IOcrEngine> CreateEngine(OcrEngineType type = OcrEngineType::kPPOCRv6);

  /**
   * @brief Create and automatically initialize the default PP-OCRv6 engine.
   * Resolves models from standard installation paths or models/ directory.
   */
  static std::unique_ptr<IOcrEngine> CreateDefaultEngine(
      const std::string& models_base_dir = "",
      OcrMode mode = OcrMode::kNormal);
};

}  // namespace ocr
}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_OCR_OCR_ENGINE_FACTORY_H_
