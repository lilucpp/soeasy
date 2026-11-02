// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/ocr/image_filter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/encoding_utils.h"

namespace soeasy {
namespace worker {
namespace ocr {

ImageFilter::ImageFilter(FilterOptions options) : options_(options) {}

FilterDecision ImageFilter::PrecheckFile(const std::string& file_path, uint64_t file_size) const {
  if (options_.filter_cache_thumbnails && IsCacheOrThumbnailPath(file_path)) {
    return FilterDecision::kIconOrThumbnailPath;
  }

  if (file_size == 0) {
    if (!common::EncodingUtils::GetFileStat(file_path, &file_size, nullptr) || file_size == 0) {
      return FilterDecision::kUnreadable;
    }
  }

  if (file_size > options_.max_file_size) {
    return FilterDecision::kFileTooLarge;
  }

  // Fast probe header dimensions
  ImageDimension dim = ProbeHeaderDimensions(file_path);
  if (dim.valid) {
    if (dim.width <= options_.min_side && dim.height <= options_.min_side) {
      return FilterDecision::kTooSmallIconOrThumbnail;
    }
    if (dim.width > 0 && dim.height > 0) {
      float aspect = static_cast<float>(dim.width) / static_cast<float>(dim.height);
      if (aspect > options_.max_aspect_ratio || aspect < (1.0f / options_.max_aspect_ratio)) {
        return FilterDecision::kExtremeAspectRatioStrip;
      }
    }
  }

  return FilterDecision::kPass;
}

bool ImageFilter::IsCacheOrThumbnailPath(const std::string& file_path) {
  std::string lower = file_path;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  static const char* kPathKeywords[] = {
      "/weappicon/", "/thumb/", "/thumbs/", "/emoji/", "/emojis/",
      "/emoticon/",  "/emoticons/", "/avatar/", "/avatars/"
  };
  for (const char* kw : kPathKeywords) {
    if (lower.find(kw) != std::string::npos) {
      return true;
    }
  }

  // Suffix checks on filename
  if (lower.find("_thumb.") != std::string::npos ||
      lower.find("_thumbnail.") != std::string::npos) {
    return true;
  }

  return false;
}

size_t ImageFilter::CountEffectiveChars(const std::string& text) {
  size_t count = 0;
  for (size_t i = 0; i < text.size(); ) {
    unsigned char c = text[i];
    size_t seq_len = 1;
    if (c >= 0xF0) seq_len = 4;
    else if (c >= 0xE0) seq_len = 3;
    else if (c >= 0xC0) seq_len = 2;

    if (seq_len == 1) {
      if (!isspace(c) && !ispunct(c) && c != '`' && c != '~' && c != '^') {
        count++;
      }
    } else if (seq_len == 3 && i + 2 < text.size()) {
      unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
      unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
      bool is_cjk_punct = false;
      if (c == 0xEF && c1 == 0xBC) {
        if (c2 >= 0x81 && c2 <= 0x9F) is_cjk_punct = true; // Full-width ASCII symbols/punct
      } else if (c == 0xE3 && c1 == 0x80) {
        if (c2 >= 0x81 && c2 <= 0x8F) is_cjk_punct = true; // CJK punctuation marks
      }
      if (!is_cjk_punct) {
        count++;
      }
    } else {
      count++;
    }
    i += seq_len;
  }
  return count;
}

FilterDecision ImageFilter::CheckDecodedImage(const cv::Mat& bgr_image) const {
  if (bgr_image.empty()) {
    return FilterDecision::kUnreadable;
  }

  if (bgr_image.cols <= options_.min_side && bgr_image.rows <= options_.min_side) {
    return FilterDecision::kTooSmallIconOrThumbnail;
  }

  float aspect = static_cast<float>(bgr_image.cols) / static_cast<float>(bgr_image.rows);
  if (aspect > options_.max_aspect_ratio || aspect < (1.0f / options_.max_aspect_ratio)) {
    return FilterDecision::kExtremeAspectRatioStrip;
  }

  // Tier 2: Check pixel variance (stddev)
  cv::Scalar mean, stddev;
  cv::meanStdDev(bgr_image, mean, stddev);
  if (stddev[0] < options_.min_std_dev &&
      stddev[1] < options_.min_std_dev &&
      stddev[2] < options_.min_std_dev) {
    return FilterDecision::kSolidColorOrBlank;
  }

  return FilterDecision::kPass;
}

ImageDimension ImageFilter::ProbeHeaderDimensions(const std::string& file_path) {
  ImageDimension dim;
  auto f = common::EncodingUtils::OpenIfstream(file_path, std::ios::binary);
  if (!f.is_open()) return dim;

  uint8_t buf[256];
  f.read(reinterpret_cast<char*>(buf), sizeof(buf));
  size_t bytes = static_cast<size_t>(f.gcount());
  if (bytes < 10) return dim;

  // 1. PNG: 89 50 4E 47 0D 0A 1A 0A -> IHDR width at 16..19, height at 20..23 (Big Endian)
  if (bytes >= 24 && buf[0] == 0x89 && buf[1] == 0x50 && buf[2] == 0x4E && buf[3] == 0x47) {
    dim.width = (buf[16] << 24) | (buf[17] << 16) | (buf[18] << 8) | buf[19];
    dim.height = (buf[20] << 24) | (buf[21] << 16) | (buf[22] << 8) | buf[23];
    dim.valid = (dim.width > 0 && dim.height > 0);
    return dim;
  }

  // 2. BMP: 42 4D ("BM") -> width at 18..21, height at 22..25 (Little Endian)
  if (bytes >= 26 && buf[0] == 'B' && buf[1] == 'M') {
    dim.width = buf[18] | (buf[19] << 8) | (buf[20] << 16) | (buf[21] << 24);
    dim.height = std::abs(static_cast<int>(buf[22] | (buf[23] << 8) | (buf[24] << 16) | (buf[25] << 24)));
    dim.valid = (dim.width > 0 && dim.height > 0);
    return dim;
  }

  // 3. GIF: "GIF87a" or "GIF89a" -> width at 6..7, height at 8..9 (Little Endian)
  if (bytes >= 10 && buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F') {
    dim.width = buf[6] | (buf[7] << 8);
    dim.height = buf[8] | (buf[9] << 8);
    dim.valid = (dim.width > 0 && dim.height > 0);
    return dim;
  }

  // 4. WebP: RIFF ... WEBP
  if (bytes >= 30 && buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' &&
      buf[8] == 'W' && buf[9] == 'E' && buf[10] == 'B' && buf[11] == 'P') {
    // VP8
    if (buf[12] == 'V' && buf[13] == 'P' && buf[14] == '8' && buf[15] == ' ') {
      dim.width = (buf[26] | (buf[27] << 8)) & 0x3FFF;
      dim.height = (buf[28] | (buf[29] << 8)) & 0x3FFF;
      dim.valid = (dim.width > 0 && dim.height > 0);
      return dim;
    }
    // VP8L (lossless)
    if (buf[12] == 'V' && buf[13] == 'P' && buf[14] == '8' && buf[15] == 'L' && bytes >= 25) {
      uint32_t b0 = buf[21], b1 = buf[22], b2 = buf[23], b3 = buf[24];
      dim.width = 1 + (((b1 & 0x3F) << 8) | b0);
      dim.height = 1 + (((b3 & 0x0F) << 10) | (b2 << 2) | ((b1 & 0xC0) >> 6));
      dim.valid = (dim.width > 0 && dim.height > 0);
      return dim;
    }
    // VP8X (extended)
    if (buf[12] == 'V' && buf[13] == 'P' && buf[14] == '8' && buf[15] == 'X' && bytes >= 30) {
      dim.width = 1 + (buf[24] | (buf[25] << 8) | (buf[26] << 16));
      dim.height = 1 + (buf[27] | (buf[28] << 8) | (buf[29] << 16));
      dim.valid = (dim.width > 0 && dim.height > 0);
      return dim;
    }
  }

  // 5. JPEG: scan markers for SOF0 (0xFF, 0xC0) or SOF2 (0xFF, 0xC2)
  if (bytes >= 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
    f.seekg(2, std::ios::beg);
    while (f) {
      uint8_t marker_buf[4];
      f.read(reinterpret_cast<char*>(marker_buf), 4);
      if (f.gcount() < 4) break;
      if (marker_buf[0] != 0xFF) break;

      uint8_t marker = marker_buf[1];
      uint16_t length = (marker_buf[2] << 8) | marker_buf[3];
      if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
        // Read SOF: precision(1), height(2), width(2)
        uint8_t sof[5];
        f.read(reinterpret_cast<char*>(sof), 5);
        if (f.gcount() == 5) {
          dim.height = (sof[1] << 8) | sof[2];
          dim.width = (sof[3] << 8) | sof[4];
          dim.valid = (dim.width > 0 && dim.height > 0);
          return dim;
        }
        break;
      }
      if (length < 2) break;
      f.seekg(length - 2, std::ios::cur);
    }
  }

  return dim;
}

}  // namespace ocr
}  // namespace worker
}  // namespace soeasy
