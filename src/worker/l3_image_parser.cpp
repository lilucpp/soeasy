// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/l3_image_parser.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "common/logger.h"
#include "worker/ocr/ocr_engine_factory.h"

namespace soeasy {
namespace worker {

L3ImageParser::L3ImageParser(ocr::OcrMode mode, const std::string& models_dir) {
  supported_exts_ = {
      "png", "jpg", "jpeg", "bmp", "webp", "tiff", "tif", "gif",
      "svg", "pnm", "ppm", "pgm", "tga"
  };

  ocr_engine_ = ocr::OcrEngineFactory::CreateDefaultEngine(models_dir, mode);
  if (!ocr_engine_) {
    LOG_WARN("[L3ImageParser] Default OCR engine not loaded yet. Will attempt lazy init on demand.");
  }
}

bool L3ImageParser::IsSupported(const std::string& ext) const {
  std::string lower_ext = ext;
  std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);
  return supported_exts_.find(lower_ext) != supported_exts_.end();
}

void L3ImageParser::SetMode(ocr::OcrMode mode) {
  if (ocr_engine_) {
    ocr_engine_->SetMode(mode);
  }
}

bool L3ImageParser::ParseSvgText(const std::string& file_path, std::string* out_utf8) {
  if (!out_utf8) return false;
  out_utf8->clear();

  std::ifstream f(file_path);
  if (!f.is_open()) return false;

  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

  // Extract content between <text...>...</text> and <tspan...>...</tspan>
  static const std::regex text_tag_regex("<(?:text|tspan)[^>]*>([^<]+)<\\/(?:text|tspan)>",
                                         std::regex_constants::icase);

  std::string result;
  auto begin = std::sregex_iterator(content.begin(), content.end(), text_tag_regex);
  auto end = std::sregex_iterator();

  for (auto it = begin; it != end; ++it) {
    std::string matched = (*it)[1].str();
    // Trim leading/trailing whitespace
    size_t start = matched.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
      size_t finish = matched.find_last_not_of(" \t\r\n");
      std::string clean = matched.substr(start, finish - start + 1);
      if (!clean.empty()) {
        if (!result.empty()) result += " ";
        result += clean;
      }
    }
  }

  *out_utf8 = result;
  return true;
}

bool L3ImageParser::Parse(const std::string& file_path, const std::string& ext,
                          std::string* out_utf8_text) {
  if (!out_utf8_text) return false;
  out_utf8_text->clear();

  std::string lower_ext = ext;
  std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);

  // 1. Vector graphics SVG: direct zero-overhead XML extraction
  if (lower_ext == "svg") {
    return ParseSvgText(file_path, out_utf8_text);
  }

  // 2. Scientific Tier 1 filtering (Header & Dimension check)
  auto decision = filter_.PrecheckFile(file_path);
  if (decision != ocr::FilterDecision::kPass) {
    LOG_INFO("[L3ImageParser] Filtered image '{}' before decoding (reason: {})",
             file_path, static_cast<int>(decision));
    return true;  // Clean skip
  }

  // Ensure OCR engine is loaded
  if (!ocr_engine_) {
    ocr_engine_ = ocr::OcrEngineFactory::CreateDefaultEngine();
    if (!ocr_engine_) {
      LOG_ERROR("[L3ImageParser] OCR Engine unavailable. Cannot parse image '{}'", file_path);
      return false;
    }
  }

  // 3. Multi-page TIFF handling
  if (lower_ext == "tiff" || lower_ext == "tif") {
    std::vector<cv::Mat> pages;
    if (cv::imreadmulti(file_path, pages) && !pages.empty()) {
      std::string full_text;
      for (size_t i = 0; i < pages.size(); ++i) {
        if (filter_.CheckDecodedImage(pages[i]) != ocr::FilterDecision::kPass) continue;
        auto ocr_res = ocr_engine_->Recognize(pages[i]);
        if (ocr_res.success && !ocr_res.full_text.empty()) {
          if (!full_text.empty()) full_text += "\n";
          full_text += ocr_res.full_text;
        }
      }
      *out_utf8_text = full_text;
      return true;
    }
  }

  // 4. Standard raster bitmap decoding
  cv::Mat img = cv::imread(file_path);
  if (img.empty()) {
    LOG_ERROR("[L3ImageParser] Failed to decode image file '{}'", file_path);
    return false;
  }

  // 5. Scientific Tier 2 filtering (Solid Color / Variance check)
  decision = filter_.CheckDecodedImage(img);
  if (decision != ocr::FilterDecision::kPass) {
    LOG_INFO("[L3ImageParser] Filtered image '{}' after decoding (reason: {})",
             file_path, static_cast<int>(decision));
    return true;  // Clean skip
  }

  // 6. Execute OCR Recognition
  auto ocr_res = ocr_engine_->Recognize(img);
  if (!ocr_res.success) {
    LOG_ERROR("[L3ImageParser] OCR recognition failed for '{}'", file_path);
    return false;
  }

  *out_utf8_text = ocr_res.full_text;
  return true;
}

}  // namespace worker
}  // namespace soeasy
