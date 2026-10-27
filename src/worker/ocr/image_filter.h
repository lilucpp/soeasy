// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_OCR_IMAGE_FILTER_H_
#define SOEASY_SRC_WORKER_OCR_IMAGE_FILTER_H_

#include <cstdint>
#include <string>
#include <opencv2/core.hpp>

namespace soeasy {
namespace worker {
namespace ocr {

enum class FilterDecision {
  kPass = 0,
  kTooSmallIconOrThumbnail = 1,
  kExtremeAspectRatioStrip = 2,
  kFileTooLarge = 3,
  kSolidColorOrBlank = 4,
  kUnreadable = 5,
  kIconOrThumbnailPath = 6
};

struct ImageDimension {
  int width = 0;
  int height = 0;
  bool valid = false;
};

struct FilterOptions {
  int min_side = 120;             // Below 120x120 is treated as icon/avatar/emoji
  float max_aspect_ratio = 20.0f; // Aspect ratio > 20 is divider/border stripe
  uint64_t max_file_size = 40ULL * 1024ULL * 1024ULL; // 40 MB max
  double min_std_dev = 8.0;       // stddev < 8 is solid single color
  bool filter_cache_thumbnails = true; // Pre-filter WeAppIcon, Thumb, Emoji, etc.
};

class ImageFilter {
 public:
  using FilterOptions = soeasy::worker::ocr::FilterOptions;

  explicit ImageFilter(FilterOptions options = FilterOptions{});

  /**
   * @brief Tier 1 fast probe: check file size, magic header, path patterns, and dimensions without decoding image.
   * Execution time < 30 microseconds.
   */
  FilterDecision PrecheckFile(const std::string& file_path, uint64_t file_size = 0) const;

  /**
   * @brief Tier 2 variance probe: check if decoded image is solid color / blank canvas.
   */
  FilterDecision CheckDecodedImage(const cv::Mat& bgr_image) const;

  /**
   * @brief Helper to probe image width and height directly from file header bytes.
   */
  static ImageDimension ProbeHeaderDimensions(const std::string& file_path);

  /**
   * @brief Check whether file path matches temporary chat/app cache, icon, or thumbnail patterns.
   */
  static bool IsCacheOrThumbnailPath(const std::string& file_path);

  /**
   * @brief Count informative non-whitespace, non-punctuation UTF-8 characters.
   */
  static size_t CountEffectiveChars(const std::string& text);

 private:
  FilterOptions options_;
};

}  // namespace ocr
}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_OCR_IMAGE_FILTER_H_
