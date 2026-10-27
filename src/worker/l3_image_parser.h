// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_L3_IMAGE_PARSER_H_
#define SOEASY_SRC_WORKER_L3_IMAGE_PARSER_H_

#include <memory>
#include <string>
#include <unordered_set>

#include "worker/ocr/i_ocr_engine.h"
#include "worker/ocr/image_filter.h"
#include "worker/ocr/ocr_types.h"

namespace soeasy {
namespace worker {

class L3ImageParser {
 public:
  explicit L3ImageParser(ocr::OcrMode mode = ocr::OcrMode::kNormal,
                         const std::string& models_dir = "");
  ~L3ImageParser() = default;

  // Disallow copy and assign
  L3ImageParser(const L3ImageParser&) = delete;
  L3ImageParser& operator=(const L3ImageParser&) = delete;

  /**
   * @brief Check if file extension is an image format handled by L3ImageParser.
   */
  bool IsSupported(const std::string& ext) const;

  /**
   * @brief Parse an image file into recognized plain text via OCR or XML parsing.
   *
   * @param file_path Canonical filesystem path
   * @param ext File extension in lowercase
   * @param out_utf8_text Output extracted text string
   * @return true on success (or clean skip on filtered icons/blanks), false on fatal read error
   */
  bool Parse(const std::string& file_path, const std::string& ext,
             std::string* out_utf8_text);

  /**
   * @brief Toggle resource mode between Normal (<=1 thread) and HighPerformance.
   */
  void SetMode(ocr::OcrMode mode);

  /**
   * @brief Parse SVG XML to extract text nodes directly without OCR rasterization.
   */
  static bool ParseSvgText(const std::string& file_path, std::string* out_utf8);

 private:
  std::unordered_set<std::string> supported_exts_;
  ocr::ImageFilter filter_;
  std::unique_ptr<ocr::IOcrEngine> ocr_engine_;
};

}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_L3_IMAGE_PARSER_H_
