// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_OCR_PPOCR_V6_ENGINE_H_
#define SOEASY_SRC_WORKER_OCR_PPOCR_V6_ENGINE_H_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>

#include "worker/ocr/i_ocr_engine.h"

namespace soeasy {
namespace worker {
namespace ocr {

class PPOCRv6Engine : public IOcrEngine {
 public:
  PPOCRv6Engine();
  ~PPOCRv6Engine() override = default;

  bool Initialize(const OcrConfig& config) override;
  OcrResult Recognize(const cv::Mat& bgr_image) override;
  OcrResult RecognizeFile(const std::string& file_path) override;
  void SetMode(OcrMode mode) override;
  OcrEngineType GetType() const override { return OcrEngineType::kPPOCRv6; }

  static cv::Size DetResize(cv::Mat& img, int limit_side_len = 736,
                            int max_side_limit = 4000, int align = 32);
  static std::vector<float> DetPreprocess(const cv::Mat& img_bgr);
  static cv::Mat GetRotateCrop(const cv::Mat& img, const std::vector<OcrPoint>& pts);
  static std::vector<float> RecPreprocess(const cv::Mat& crop_bgr, int target_h = 48,
                                          int target_w = 320, int max_img_w = 3200);
  static std::string CtcDecode(const float* logits, int T, int C,
                               const std::vector<std::string>& char_dict,
                               float* out_conf = nullptr);

 private:
  OcrConfig config_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> det_session_;
  std::unique_ptr<Ort::Session> rec_session_;
  std::unique_ptr<Ort::MemoryInfo> mem_info_;
  std::vector<std::string> dict_;
  std::mutex mutex_;
  bool initialized_ = false;

  void ApplySessionThreads(Ort::SessionOptions& opts, OcrMode mode);
};

}  // namespace ocr
}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_OCR_PPOCR_V6_ENGINE_H_
