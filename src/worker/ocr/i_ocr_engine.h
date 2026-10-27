// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_OCR_I_OCR_ENGINE_H_
#define SOEASY_SRC_WORKER_OCR_I_OCR_ENGINE_H_

#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "worker/ocr/ocr_types.h"

namespace soeasy {
namespace worker {
namespace ocr {

/**
 * @brief Abstract interface for OCR recognition engines.
 * Enables clean decoupling and seamless hot-swapping between OCR backends.
 */
class IOcrEngine {
 public:
  virtual ~IOcrEngine() = default;

  /**
   * @brief Initialize or re-initialize the engine with given configuration.
   */
  virtual bool Initialize(const OcrConfig& config) = 0;

  /**
   * @brief Perform OCR recognition on an in-memory BGR image matrix.
   */
  virtual OcrResult Recognize(const cv::Mat& bgr_image) = 0;

  /**
   * @brief Perform OCR recognition on an image file on disk.
   */
  virtual OcrResult RecognizeFile(const std::string& file_path) = 0;

  /**
   * @brief Dynamically toggle resource mode between Normal (1 thread) and HighPerformance.
   */
  virtual void SetMode(OcrMode mode) = 0;

  /**
   * @brief Get the type of this OCR engine.
   */
  virtual OcrEngineType GetType() const = 0;
};

}  // namespace ocr
}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_OCR_I_OCR_ENGINE_H_
