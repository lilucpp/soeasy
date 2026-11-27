// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/l3_image_parser.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <fstream>
#include <opencv2/imgcodecs.hpp>
#include <regex>
#include <string_view>
#include <vector>

#include "common/encoding_utils.h"
#include "common/format_categories.h"
#include "common/logger.h"
#include "common/system_env.h"
#include "worker/ocr/ocr_engine_factory.h"

namespace soeasy {
namespace worker {

L3ImageParser::L3ImageParser(ocr::OcrMode mode, const std::string &models_dir)
    : mode_(mode),
      models_dir_(models_dir),
      last_ocr_active_time_(std::chrono::steady_clock::now()) {
  // Lazy init: ocr_engine_ remains nullptr until actual image OCR is required.
}

void L3ImageParser::EnsureOcrEngineLoaded() {
  if (!ocr_engine_) {
    ocr_engine_ = ocr::OcrEngineFactory::CreateDefaultEngine(models_dir_, mode_);
    if (!ocr_engine_) {
      LOG_WARN("[L3ImageParser] Failed to instantiate default OCR engine on demand.");
    } else {
      LOG_INFO("[L3ImageParser] PP-OCRv6 Engine lazily initialized on demand.");
    }
  }
}

void L3ImageParser::CheckIdleUnload(double idle_seconds) {
  if (!ocr_engine_) {
    return;
  }
  auto now = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(now - last_ocr_active_time_).count();
  if (elapsed >= idle_seconds) {
    ocr_engine_.reset();
    LOG_INFO("[L3ImageParser] OCR engine idle for {:.1f}s. Unloaded from memory to reclaim RAM.", elapsed);
    common::SystemEnv::TrimHeap();
  }
}

bool L3ImageParser::IsSupported(const std::string &ext) const {
  return common::IsSupportedImageExtension(ext);
}

void L3ImageParser::SetMode(ocr::OcrMode mode) {
  if (ocr_engine_) {
    ocr_engine_->SetMode(mode);
  }
}

namespace {

void ExtractSvgTextFromContent(std::string_view content, std::string *out_utf8) {
  static const std::regex text_tag_regex("<(?:text|tspan)[^>]*>([^<]+)<\\/(?:text|tspan)>",
                                         std::regex_constants::icase);

  std::string result;
  auto begin = std::cregex_iterator(content.data(), content.data() + content.size(), text_tag_regex);
  auto end = std::cregex_iterator();

  for (auto it = begin; it != end; ++it) {
    std::string matched = (*it)[1].str();
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
  *out_utf8 = std::move(result);
}

}  // namespace

bool L3ImageParser::ParseSvgText(const std::string &file_path, std::string *out_utf8) {
  if (!out_utf8) return false;
  out_utf8->clear();

  auto ifs = common::EncodingUtils::OpenIfstream(file_path);
  if (!ifs.is_open()) return false;
  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  ExtractSvgTextFromContent(content, out_utf8);
  return true;
}

bool L3ImageParser::Parse(const std::string &file_path, const std::string &ext, std::string *out_utf8_text) {
  return ParseInternal(file_path, nullptr, 0, ext, out_utf8_text, nullptr);
}

bool L3ImageParser::RecognizeSingleImage(const std::string &file_path, const cv::Mat &raw_img,
                                         std::string *out_utf8_text, std::string *out_error) {
  cv::Mat img = ocr::ImageFilter::NormalizeToBgr(raw_img);
  if (img.empty()) {
    LOG_ERROR("[L3ImageParser] Failed to decode image '{}'", file_path);
    if (out_error) *out_error = "Failed to decode raster image";
    return false;
  }

  auto decision = filter_.CheckDecodedImage(img);
  if (decision != ocr::FilterDecision::kPass) {
    LOG_INFO("[L3ImageParser] Filtered image '{}' after decoding (reason: {})", file_path, static_cast<int>(decision));
    return true;  // Clean skip
  }

  auto ocr_res = ocr_engine_->Recognize(img);
  if (!ocr_res.success) {
    LOG_ERROR("[L3ImageParser] OCR recognition failed for '{}'", file_path);
    if (out_error) *out_error = "OCR recognition failed";
    return false;
  }

  size_t eff_chars = ocr::ImageFilter::CountEffectiveChars(ocr_res.full_text);
  if (eff_chars < min_effective_chars_) {
    LOG_INFO(
        "[L3ImageParser] Filtered image '{}': insufficient text ({} effective chars < {}, {} boxes), skipping body "
        "index",
        file_path, eff_chars, min_effective_chars_, ocr_res.lines.size());
    out_utf8_text->clear();
    return true;
  }

  *out_utf8_text = ocr_res.full_text;
  return true;
}

bool L3ImageParser::ParseInternal(const std::string &file_path, const uint8_t *raw_bytes, size_t raw_size,
                                  const std::string &ext, std::string *out_utf8_text, std::string *out_error) {
  if (!out_utf8_text) return false;
  out_utf8_text->clear();
  if (out_error) out_error->clear();

  std::string lower_ext = ext;
  std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });

  // 1. Vector graphics SVG: direct zero-overhead XML extraction
  if (lower_ext == "svg") {
    bool ok = false;
    if (raw_bytes != nullptr) {
      ExtractSvgTextFromContent(std::string_view(reinterpret_cast<const char *>(raw_bytes), raw_size), out_utf8_text);
      ok = true;
    } else {
      ok = ParseSvgText(file_path, out_utf8_text);
      if (!ok && out_error) {
        *out_error = "Cannot open SVG file for reading";
      }
    }
    if (ok && min_effective_chars_ > 0 &&
        ocr::ImageFilter::CountEffectiveChars(*out_utf8_text) < min_effective_chars_) {
      out_utf8_text->clear();
    }
    return ok;
  }

  // 2. Scientific Tier 1 filtering (Header & Dimension check)
  auto decision = (raw_bytes != nullptr) ? filter_.PrecheckBuffer(file_path, raw_bytes, raw_size)
                                         : filter_.PrecheckFile(file_path);
  if (decision != ocr::FilterDecision::kPass) {
    LOG_INFO("[L3ImageParser] Filtered image '{}' before decoding (reason: {})", file_path, static_cast<int>(decision));
    return true;  // Clean skip
  }

  // Ensure OCR engine is loaded on demand
  EnsureOcrEngineLoaded();
  if (!ocr_engine_) {
    LOG_ERROR("[L3ImageParser] OCR Engine unavailable. Cannot parse image '{}'", file_path);
    if (out_error) *out_error = "OCR engine unavailable";
    return false;
  }
  last_ocr_active_time_ = std::chrono::steady_clock::now();

  // 3. Multi-page TIFF handling (file path mode; memory buffer decodes via cv::imdecode below)
  if (raw_bytes == nullptr && (lower_ext == "tiff" || lower_ext == "tif")) {
    std::vector<cv::Mat> pages;
    if (cv::imreadmulti(file_path, pages, cv::IMREAD_UNCHANGED) && !pages.empty()) {
      std::string full_text;
      for (size_t i = 0; i < pages.size(); ++i) {
        cv::Mat bgr_page = ocr::ImageFilter::NormalizeToBgr(pages[i]);
        if (filter_.CheckDecodedImage(bgr_page) != ocr::FilterDecision::kPass) continue;
        auto ocr_res = ocr_engine_->Recognize(bgr_page);
        if (ocr_res.success && !ocr_res.full_text.empty()) {
          if (!full_text.empty()) full_text += "\n";
          full_text += ocr_res.full_text;
        }
      }
      if (min_effective_chars_ > 0 && ocr::ImageFilter::CountEffectiveChars(full_text) < min_effective_chars_) {
        full_text.clear();
      }
      *out_utf8_text = full_text;
      return true;
    }
  }

  // 4. Raster bitmap decoding (from memory buffer or disk file)
  cv::Mat raw_img;
  if (raw_bytes != nullptr) {
    if (raw_size == 0 || raw_size > static_cast<size_t>(INT_MAX)) {
      if (out_error) *out_error = "Invalid image buffer size";
      return false;
    }
    cv::Mat buf_view(1, static_cast<int>(raw_size), CV_8UC1, const_cast<uint8_t *>(raw_bytes));
    raw_img = cv::imdecode(buf_view, cv::IMREAD_UNCHANGED);
  } else {
    std::string native_path = common::EncodingUtils::ToNativeAnsiPath(file_path);
    raw_img = cv::imread(native_path, cv::IMREAD_UNCHANGED);
  }

  return RecognizeSingleImage(file_path, raw_img, out_utf8_text, out_error);
}

bool L3ImageParser::Parse(const ParseContext &ctx, ExtractedDocument *out_doc) {
  if (!out_doc) {
    return false;
  }
  out_doc->is_scanned_pdf = false;
  out_doc->page_count = 1;
  out_doc->outcome = ParseOutcome::kOk;
  out_doc->error_message.clear();
  if (!ParseInternal(ctx.file_path, nullptr, 0, ctx.ext, &out_doc->utf8_text, &out_doc->error_message)) {
    out_doc->outcome = ParseOutcome::kFailed;
    return false;
  }
  return true;
}

bool L3ImageParser::ParseBuffer(const char *raw_bytes, size_t raw_size, const ParseContext &ctx,
                                ExtractedDocument *out_doc) {
  if (!out_doc) {
    return false;
  }
  out_doc->is_scanned_pdf = false;
  out_doc->page_count = 1;
  out_doc->outcome = ParseOutcome::kOk;
  out_doc->error_message.clear();
  const auto *bytes_ptr = raw_bytes ? reinterpret_cast<const uint8_t *>(raw_bytes)
                                    : reinterpret_cast<const uint8_t *>("");
  if (!ParseInternal(ctx.file_path, bytes_ptr, raw_size, ctx.ext, &out_doc->utf8_text, &out_doc->error_message)) {
    out_doc->outcome = ParseOutcome::kFailed;
    return false;
  }
  return true;
}

bool L3ImageParser::ParseStream(std::istream &stream, const ParseContext &ctx, ExtractedDocument *out_doc) {
  if (!out_doc) {
    return false;
  }
  std::string buffer;
  constexpr size_t kMaxStreamSize = 128ULL * 1024 * 1024;
  if (ctx.file_size > 0 && ctx.file_size <= kMaxStreamSize) {
    buffer.reserve(ctx.file_size);
  }
  char chunk[65536];
  while (stream.read(chunk, sizeof(chunk)) || stream.gcount() > 0) {
    const size_t count = static_cast<size_t>(stream.gcount());
    if (buffer.size() + count > kMaxStreamSize) {
      out_doc->outcome = ParseOutcome::kFailed;
      out_doc->error_message = "Image input stream exceeds maximum buffer size";
      return false;
    }
    buffer.append(chunk, count);
  }
  return ParseBuffer(buffer.data(), buffer.size(), ctx, out_doc);
}

}  // namespace worker
}  // namespace soeasy
