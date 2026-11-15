// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/ocr/image_filter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <opencv2/imgproc.hpp>
#include <vector>

#include "common/encoding_utils.h"

namespace soeasy {
namespace worker {
namespace ocr {

ImageFilter::ImageFilter(FilterOptions options) : options_(options) {}

FilterDecision ImageFilter::CheckDimensions(int width, int height) const {
  if (width <= 0 || height <= 0) {
    return FilterDecision::kUnreadable;
  }
  if (width <= options_.min_side && height <= options_.min_side) {
    return FilterDecision::kTooSmallIconOrThumbnail;
  }
  const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
  if (options_.max_image_pixels > 0 && pixels > options_.max_image_pixels) {
    return FilterDecision::kTooManyPixels;
  }
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  if (aspect > options_.max_aspect_ratio || aspect < (1.0f / options_.max_aspect_ratio)) {
    return FilterDecision::kExtremeAspectRatioStrip;
  }
  return FilterDecision::kPass;
}

FilterDecision ImageFilter::PrecheckFile(const std::string &file_path, uint64_t file_size) const {
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
    return CheckDimensions(dim.width, dim.height);
  }

  return FilterDecision::kPass;
}

bool ImageFilter::IsCacheOrThumbnailPath(const std::string &file_path) {
  std::string lower = file_path;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  std::replace(lower.begin(), lower.end(), '\\', '/');

  static const char *kPathKeywords[] = {"/weappicon/", "/thumb/",     "/thumbs/", "/emoji/",  "/emojis/",
                                        "/emoticon/",  "/emoticons/", "/avatar/", "/avatars/"};
  for (const char *kw : kPathKeywords) {
    if (lower.find(kw) != std::string::npos) {
      return true;
    }
  }

  // Suffix checks on filename
  if (lower.find("_thumb.") != std::string::npos || lower.find("_thumbnail.") != std::string::npos) {
    return true;
  }

  return false;
}

size_t ImageFilter::CountEffectiveChars(const std::string &text) {
  size_t count = 0;
  for (size_t i = 0; i < text.size();) {
    unsigned char c = text[i];
    size_t seq_len = 1;
    if (c >= 0xF0)
      seq_len = 4;
    else if (c >= 0xE0)
      seq_len = 3;
    else if (c >= 0xC0)
      seq_len = 2;

    if (seq_len == 1) {
      if (!isspace(c) && !ispunct(c) && c != '`' && c != '~' && c != '^') {
        count++;
      }
    } else if (seq_len == 3 && i + 2 < text.size()) {
      unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
      unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
      bool is_cjk_punct = false;
      if (c == 0xEF && c1 == 0xBC) {
        if (c2 >= 0x81 && c2 <= 0x9F) is_cjk_punct = true;  // Full-width ASCII symbols/punct
      } else if (c == 0xE3 && c1 == 0x80) {
        if (c2 >= 0x81 && c2 <= 0x8F) is_cjk_punct = true;  // CJK punctuation marks
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

cv::Mat ImageFilter::NormalizeToBgr(const cv::Mat &src) {
  if (src.empty()) {
    return cv::Mat();
  }
  cv::Mat img8;
  if (src.depth() == CV_16U) {
    src.convertTo(img8, CV_8U, 255.0 / 65535.0);
  } else if (src.depth() != CV_8U) {
    src.convertTo(img8, CV_8U);
  } else {
    img8 = src;
  }

  if (img8.channels() == 3) {
    return img8;
  }
  if (img8.channels() == 1) {
    cv::Mat bgr;
    cv::cvtColor(img8, bgr, cv::COLOR_GRAY2BGR);
    return bgr;
  }
  if (img8.channels() == 4) {
    // Composite BGRA onto solid white so transparent dark-text PNGs do not turn solid black.
    cv::Mat bgr(img8.size(), CV_8UC3);
    for (int y = 0; y < img8.rows; ++y) {
      const cv::Vec4b *sp = img8.ptr<cv::Vec4b>(y);
      cv::Vec3b *dp = bgr.ptr<cv::Vec3b>(y);
      for (int x = 0; x < img8.cols; ++x) {
        const uint8_t a = sp[x][3];
        if (a == 255) {
          dp[x] = cv::Vec3b(sp[x][0], sp[x][1], sp[x][2]);
        } else if (a == 0) {
          dp[x] = cv::Vec3b(255, 255, 255);
        } else {
          const int inv = 255 - a;
          dp[x][0] = static_cast<uint8_t>((sp[x][0] * a + 255 * inv + 127) / 255);
          dp[x][1] = static_cast<uint8_t>((sp[x][1] * a + 255 * inv + 127) / 255);
          dp[x][2] = static_cast<uint8_t>((sp[x][2] * a + 255 * inv + 127) / 255);
        }
      }
    }
    return bgr;
  }
  return cv::Mat();
}

FilterDecision ImageFilter::CheckDecodedImage(const cv::Mat &bgr_image) const {
  if (bgr_image.empty()) {
    return FilterDecision::kUnreadable;
  }

  FilterDecision dim_decision = CheckDimensions(bgr_image.cols, bgr_image.rows);
  if (dim_decision != FilterDecision::kPass) {
    return dim_decision;
  }

  // Tier 2: Check pixel variance (stddev)
  cv::Scalar mean, stddev;
  cv::meanStdDev(bgr_image, mean, stddev);
  if (bgr_image.channels() == 1) {
    if (stddev[0] < options_.min_std_dev) {
      return FilterDecision::kSolidColorOrBlank;
    }
  } else if (stddev[0] < options_.min_std_dev && stddev[1] < options_.min_std_dev && stddev[2] < options_.min_std_dev) {
    return FilterDecision::kSolidColorOrBlank;
  }

  return FilterDecision::kPass;
}

ImageDimension ImageFilter::ProbeBufferDimensions(const uint8_t *buf, size_t bytes) {
  ImageDimension dim;
  if (!buf || bytes < 10) return dim;

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
  if (bytes >= 30 && buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' && buf[8] == 'W' &&
      buf[9] == 'E' && buf[10] == 'B' && buf[11] == 'P') {
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

  // 5. JPEG: scan markers for SOF0 (0xFF, 0xC0) ... SOF3 (0xFF, 0xC3)
  if (bytes >= 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
    size_t offset = 2;
    while (offset + 4 <= bytes) {
      if (buf[offset] != 0xFF) {
        offset++;
        continue;
      }
      uint8_t marker = buf[offset + 1];
      if (marker == 0xFF || marker == 0x00) {
        offset++;
        continue;
      }
      if (marker == 0xD9 || marker == 0xDA) {  // EOI or SOS
        break;
      }
      uint16_t length = (static_cast<uint16_t>(buf[offset + 2]) << 8) | buf[offset + 3];
      if ((marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
          (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF)) {
        if (offset + 4 + 5 <= bytes) {
          dim.height = (buf[offset + 5] << 8) | buf[offset + 6];
          dim.width = (buf[offset + 7] << 8) | buf[offset + 8];
          dim.valid = (dim.width > 0 && dim.height > 0);
          return dim;
        }
        break;
      }
      if (length < 2) break;
      offset += 2 + length;
    }
  }

  return dim;
}

ImageDimension ImageFilter::ProbeHeaderDimensions(const std::string &file_path) {
  ImageDimension dim;
  auto f = common::EncodingUtils::OpenIfstream(file_path, std::ios::binary);
  if (!f.is_open()) return dim;

  std::vector<uint8_t> buf(65536);
  f.read(reinterpret_cast<char *>(buf.data()), buf.size());
  size_t bytes = static_cast<size_t>(f.gcount());
  if (bytes < 10) return dim;

  return ProbeBufferDimensions(buf.data(), bytes);
}

}  // namespace ocr
}  // namespace worker
}  // namespace soeasy
