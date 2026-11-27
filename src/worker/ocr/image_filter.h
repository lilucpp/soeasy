// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_OCR_IMAGE_FILTER_H_
#define SOEASY_SRC_WORKER_OCR_IMAGE_FILTER_H_

#include <cstdint>
#include <opencv2/core.hpp>
#include <string>

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
  kIconOrThumbnailPath = 6,
  kTooManyPixels = 7
};

struct ImageDimension {
  int width = 0;
  int height = 0;
  bool valid = false;
};

struct FilterOptions {
  static constexpr size_t kDefaultMinEffectiveChars = 4;

  int min_side = 120;                                     // Below 120x120 is treated as icon/avatar/emoji
  float max_aspect_ratio = 8.0f;                          // Aspect ratio > 8 (dividers or unreadable long strips)
  uint64_t max_file_size = 40ULL * 1024ULL * 1024ULL;     // 40 MB max
  uint64_t max_image_pixels = 36ULL * 1024ULL * 1024ULL;  // 36 MP ceiling to guard against decompression OOM
  double min_std_dev = 8.0;                               // stddev < 8 is solid single color
  size_t min_effective_chars = kDefaultMinEffectiveChars; // Minimum informative chars to keep OCR result
  bool filter_cache_thumbnails = true;                    // Pre-filter WeAppIcon, Thumb, Emoji, etc.
};

class ImageFilter {
 public:
  using FilterOptions = soeasy::worker::ocr::FilterOptions;

  explicit ImageFilter(FilterOptions options = FilterOptions{});

  const FilterOptions &options() const { return options_; }

  /**
   * @brief Tier 1 fast probe: check file size, magic header, path patterns, and dimensions without decoding image.
   * Execution time < 30 microseconds.
   */
  FilterDecision PrecheckFile(const std::string &file_path, uint64_t file_size = 0) const;

  /**
   * @brief Tier 1 fast probe on an in-memory image buffer without disk access.
   */
  FilterDecision PrecheckBuffer(const std::string &file_path, const uint8_t *buf, size_t bytes) const;

  /**
   * @brief Tier 2 variance probe: check if decoded image is solid color / blank canvas.
   */
  FilterDecision CheckDecodedImage(const cv::Mat &bgr_image) const;

  /**
   * @brief Shared dimension/aspect/pixel-budget check usable before or after image decoding.
   */
  FilterDecision CheckDimensions(int width, int height) const;

  /**
   * @brief Normalize decoded Mat (gray / 16-bit / BGRA with alpha composited onto white) to 8-bit 3-channel BGR.
   */
  static cv::Mat NormalizeToBgr(const cv::Mat &src);

  /**
   * @brief Helper to probe image width and height directly from file header bytes.
   */
  static ImageDimension ProbeHeaderDimensions(const std::string &file_path);

  /**
   * @brief Helper to probe image width and height directly from in-memory byte buffer without decoding.
   */
  static ImageDimension ProbeBufferDimensions(const uint8_t *buf, size_t bytes);

  /**
   * @brief Check whether file path matches temporary chat/app cache, icon, or thumbnail patterns.
   */
  static bool IsCacheOrThumbnailPath(const std::string &file_path);

  /**
   * @brief Count informative non-whitespace, non-punctuation UTF-8 characters.
   */
  static size_t CountEffectiveChars(const std::string &text);

 private:
  FilterOptions options_;
};

}  // namespace ocr
}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_OCR_IMAGE_FILTER_H_
