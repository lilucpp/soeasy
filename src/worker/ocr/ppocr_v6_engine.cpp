// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/ocr/ppocr_v6_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <thread>

#include "common/logger.h"
#include "common/system_env.h"
#include "worker/ocr/image_filter.h"

namespace soeasy {
namespace worker {
namespace ocr {

namespace {

struct ScopedHeapTrim {
  ~ScopedHeapTrim() { common::SystemEnv::TrimHeap(); }
};

}  // namespace

PPOCRv6Engine::PPOCRv6Engine()
    : env_(std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "PPOCRv6Engine")),
      mem_info_(std::make_unique<Ort::MemoryInfo>(Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault))) {}

void PPOCRv6Engine::ApplySessionThreads(Ort::SessionOptions &opts, OcrMode mode) {
  opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  if (mode == OcrMode::kNormal) {
    // General mode: <= 1 CPU core constraint
    opts.SetIntraOpNumThreads(1);
    opts.SetInterOpNumThreads(1);
  } else {
    // High performance mode: up to 4 threads
    unsigned int threads = std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
    opts.SetIntraOpNumThreads(static_cast<int>(threads));
    opts.SetInterOpNumThreads(1);
  }
}

bool PPOCRv6Engine::Initialize(const OcrConfig &config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;

  try {
    Ort::SessionOptions session_opts;
    ApplySessionThreads(session_opts, config_.mode);

#ifdef _WIN32
    std::wstring det_path = std::filesystem::u8path(config_.det_model_path).wstring();
    std::wstring rec_path = std::filesystem::u8path(config_.rec_model_path).wstring();
    det_session_ = std::make_unique<Ort::Session>(*env_, det_path.c_str(), session_opts);
    rec_session_ = std::make_unique<Ort::Session>(*env_, rec_path.c_str(), session_opts);
#else
    det_session_ = std::make_unique<Ort::Session>(*env_, config_.det_model_path.c_str(), session_opts);
    rec_session_ = std::make_unique<Ort::Session>(*env_, config_.rec_model_path.c_str(), session_opts);
#endif

    // Load dictionary
    dict_.clear();
    std::ifstream ifs(config_.dict_path);
    if (!ifs.is_open()) {
      LOG_ERROR("[PPOCRv6] Failed to open dictionary file: '{}'", config_.dict_path);
      return false;
    }

    std::string line;
    while (std::getline(ifs, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      dict_.push_back(line);
    }

    initialized_ = true;
    LOG_INFO("[PPOCRv6] Engine successfully initialized (dict size: {}, mode: {})", dict_.size(),
             config_.mode == OcrMode::kNormal ? "Normal (1-thread)" : "HighPerformance");
    return true;
  } catch (const Ort::Exception &ex) {
    LOG_ERROR("[PPOCRv6] Ort::Exception during initialization: {}", ex.what());
    return false;
  } catch (const std::exception &ex) {
    LOG_ERROR("[PPOCRv6] Exception during initialization: {}", ex.what());
    return false;
  }
}

void PPOCRv6Engine::SetMode(OcrMode mode) {
  if (config_.mode == mode) return;
  config_.mode = mode;
  if (initialized_) {
    Initialize(config_);
  }
}

cv::Size PPOCRv6Engine::DetResize(cv::Mat &img, int limit_side_len, int max_side_limit, int align,
                                  DetLimitType limit_type) {
  if (img.empty()) {
    return cv::Size();
  }
  int h = img.rows, w = img.cols;
  cv::Size orig(w, h);
  float ratio = 1.0f;

  if (limit_type == DetLimitType::kMax) {
    int max_side = (std::max)(h, w);
    if (limit_side_len > 0 && max_side > limit_side_len) {
      ratio = static_cast<float>(limit_side_len) / static_cast<float>(max_side);
    }
  } else {
    int min_side = (std::min)(h, w);
    if (limit_side_len > 0 && min_side < limit_side_len) {
      ratio = static_cast<float>(limit_side_len) / static_cast<float>(min_side);
    }
  }

  // Always clamp longest side with max_side_limit to prevent narrow strips from blowing up in kMin mode.
  if (max_side_limit > 0) {
    int max_side = (std::max)(h, w);
    if (static_cast<float>(max_side) * ratio > static_cast<float>(max_side_limit)) {
      ratio = static_cast<float>(max_side_limit) / static_cast<float>(max_side);
    }
  }

  int new_w = static_cast<int>(std::round(w * ratio));
  int new_h = static_cast<int>(std::round(h * ratio));
  // Align input dimensions to nearest multiple of 32 required by DBNet backbone.
  new_w = (std::max)(static_cast<int>(std::round(static_cast<double>(new_w) / align)) * align, align);
  new_h = (std::max)(static_cast<int>(std::round(static_cast<double>(new_h) / align)) * align, align);
  // Keep rounded dimensions strictly within max_side_limit when max_side_limit >= align.
  if (max_side_limit >= align) {
    if (new_w > max_side_limit) new_w = (std::max)((max_side_limit / align) * align, align);
    if (new_h > max_side_limit) new_h = (std::max)((max_side_limit / align) * align, align);
  }
  if (new_w != w || new_h != h) {
    const int interp = (new_w < w || new_h < h) ? cv::INTER_AREA : cv::INTER_CUBIC;
    cv::resize(img, img, cv::Size(new_w, new_h), 0, 0, interp);
  }
  return orig;
}

std::vector<float> PPOCRv6Engine::DetPreprocess(const cv::Mat &img_bgr) {
  cv::Mat rgb;
  cv::cvtColor(img_bgr, rgb, cv::COLOR_BGR2RGB);
  rgb.convertTo(rgb, CV_32F, 1.0 / 255.0);

  constexpr float mean[3] = {0.485f, 0.456f, 0.406f};
  constexpr float std[3] = {0.229f, 0.224f, 0.225f};
  std::vector<cv::Mat> channels(3);
  cv::split(rgb, channels);
  for (int c = 0; c < 3; ++c) channels[c] = (channels[c] - mean[c]) / std[c];

  int h = channels[0].rows, w = channels[0].cols;
  std::vector<float> data(3 * h * w);
  for (int c = 0; c < 3; ++c)
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) data[c * h * w + y * w + x] = channels[c].at<float>(y, x);
  return data;
}

cv::Mat PPOCRv6Engine::GetRotateCrop(const cv::Mat &img, const std::vector<OcrPoint> &pts) {
  if (pts.size() < 4) return cv::Mat();

  cv::Point2f p0(pts[0].x, pts[0].y);
  cv::Point2f p1(pts[1].x, pts[1].y);
  cv::Point2f p2(pts[2].x, pts[2].y);
  cv::Point2f p3(pts[3].x, pts[3].y);

  float w1 = static_cast<float>(cv::norm(p1 - p0));
  float w2 = static_cast<float>(cv::norm(p3 - p2));
  float dst_w = (std::max)(w1, w2);
  float h1 = static_cast<float>(cv::norm(p2 - p0));
  float h2 = static_cast<float>(cv::norm(p3 - p1));
  float dst_h = (std::max)(h1, h2);

  int iw = (std::max)(static_cast<int>(dst_w), 1);
  int ih = (std::max)(static_cast<int>(dst_h), 1);

  const int pad = 3;
  std::vector<cv::Point2f> src = {p0, p1, p2, p3};
  std::vector<cv::Point2f> dst = {{float(pad), float(pad)},
                                  {float(iw - 1 + pad), float(pad)},
                                  {float(pad), float(ih - 1 + pad)},
                                  {float(iw - 1 + pad), float(ih - 1 + pad)}};

  cv::Mat M = cv::getPerspectiveTransform(src, dst);
  cv::Mat crop;
  cv::warpPerspective(img, crop, M, cv::Size(iw + 2 * pad, ih + 2 * pad), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
  return crop;
}

std::vector<float> PPOCRv6Engine::RecPreprocess(const cv::Mat &crop_bgr, int target_h, int target_w, int max_img_w) {
  int h = crop_bgr.rows, w = crop_bgr.cols;
  float wh_ratio = float(w) / float(h);
  float max_wh_ratio = (std::max)(float(target_w) / float(target_h), wh_ratio);
  int img_w = int(target_h * max_wh_ratio);
  if (img_w > max_img_w) img_w = max_img_w;

  int resized_w = (std::min)(int(std::ceil(target_h * wh_ratio)), img_w);
  cv::Mat img;
  cv::resize(crop_bgr, img, cv::Size(resized_w, target_h));

  std::vector<float> data(3 * target_h * img_w, 0.f);
  for (int c = 0; c < 3; ++c)
    for (int y = 0; y < target_h; ++y)
      for (int x = 0; x < resized_w; ++x)
        data[c * target_h * img_w + y * img_w + x] = (img.at<cv::Vec3b>(y, x)[c] / 255.0f - 0.5f) / 0.5f;
  return data;
}

std::string PPOCRv6Engine::CtcDecode(const float *logits, int T, int C, const std::vector<std::string> &char_dict,
                                     float *out_conf) {
  std::string result;
  int prev = -1;
  float conf_sum = 0.0f;
  int conf_cnt = 0;

  for (int t = 0; t < T; ++t) {
    int max_c = 0;
    float max_v = logits[t * C];
    for (int c = 1; c < C; ++c) {
      if (logits[t * C + c] > max_v) {
        max_v = logits[t * C + c];
        max_c = c;
      }
    }
    if (max_c == 0) {
      prev = -1;
      continue;
    }  // blank token
    if (max_c == prev) continue;  // deduplicate CTC tokens
    if (max_c >= static_cast<int>(char_dict.size())) continue;

    result += char_dict[max_c];
    conf_sum += max_v;
    conf_cnt++;
    prev = max_c;
  }
  if (out_conf) *out_conf = (conf_cnt > 0) ? (conf_sum / conf_cnt) : 0.0f;
  return result;
}

OcrResult PPOCRv6Engine::RecognizeFile(const std::string &file_path) {
  cv::Mat raw = cv::imread(file_path, cv::IMREAD_UNCHANGED);
  cv::Mat img = ImageFilter::NormalizeToBgr(raw);
  if (img.empty()) {
    OcrResult res;
    res.success = false;
    return res;
  }
  return Recognize(img);
}

std::vector<OcrBox> PPOCRv6Engine::DetectTextBoxes(const cv::Mat &norm_bgr, const Ort::RunOptions &shrink_run_opts,
                                                   double *out_det_ms) {
  auto t0 = std::chrono::high_resolution_clock::now();

  cv::Mat det_img = norm_bgr.clone();
  cv::Size orig_size = DetResize(det_img, config_.limit_side_len, config_.max_side_limit, 32, config_.det_limit_type);
  int det_h = det_img.rows, det_w = det_img.cols;

  std::vector<float> det_in = DetPreprocess(det_img);
  std::vector<int64_t> det_shape = {1, 3, det_h, det_w};

  const char *in_names[] = {"x"};
  const char *out_names[] = {"fetch_name_0"};

  Ort::Value in_tensor =
      Ort::Value::CreateTensor<float>(*mem_info_, det_in.data(), det_in.size(), det_shape.data(), det_shape.size());

  // Run detection session and shrink CPU arena immediately after detection completes.
  auto det_out = det_session_->Run(shrink_run_opts, in_names, &in_tensor, 1, out_names, 1);
  const float *pred = det_out[0].GetTensorData<float>();

  auto t1 = std::chrono::high_resolution_clock::now();
  if (out_det_ms) {
    *out_det_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  }

  // Postprocess detection map to boxes
  cv::Mat bin_map(det_h, det_w, CV_8U);
  for (int i = 0; i < det_h * det_w; ++i) bin_map.data[i] = (pred[i] > config_.det_db_thresh) ? 255 : 0;

  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(bin_map, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);

  float scale_w = float(orig_size.width) / float(det_w);
  float scale_h = float(orig_size.height) / float(det_h);

  std::vector<OcrBox> boxes;
  for (const auto &cnt : contours) {
    if (cnt.size() < 4) continue;
    cv::RotatedRect min_rect = cv::minAreaRect(cnt);
    if ((std::min)(min_rect.size.width, min_rect.size.height) < 3.0f) continue;

    float score = 0.f;
    {
      cv::Rect bbox = cv::boundingRect(cnt);
      bbox &= cv::Rect(0, 0, det_w, det_h);
      if (bbox.area() <= 0) continue;
      cv::Mat crop_mask = cv::Mat::zeros(bbox.size(), CV_8U);
      std::vector<cv::Point> shifted = cnt;
      for (auto &p : shifted) p -= bbox.tl();
      cv::drawContours(crop_mask, std::vector<std::vector<cv::Point>>{shifted}, 0, cv::Scalar(255), cv::FILLED);
      float sum = 0.f;
      int count = 0;
      for (int y = 0; y < bbox.height; ++y) {
        for (int x = 0; x < bbox.width; ++x) {
          if (crop_mask.at<uchar>(y, x) == 255) {
            sum += pred[(bbox.y + y) * det_w + (bbox.x + x)];
            count++;
          }
        }
      }
      if (count > 0) score = sum / count;
    }
    if (score < config_.det_box_thresh) continue;

    double peri = cv::arcLength(cnt, true);
    std::vector<cv::Point> approx;
    cv::approxPolyDP(cnt, approx, 0.002 * peri, true);
    if (approx.size() < 4) continue;

    double area = cv::contourArea(approx);
    double perimeter = cv::arcLength(approx, true);
    double distance = (perimeter > 1e-4) ? (area * config_.unclip_ratio / perimeter) : 0;

    int ksize = (std::max)(1, (int)std::ceil(distance));
    // Dilate only within padded bounding ROI to avoid allocating a full det_h x det_w mask per box.
    const int pad = ksize + 2;
    cv::Rect roi = cv::boundingRect(approx);
    const int rx = (std::max)(0, roi.x - pad);
    const int ry = (std::max)(0, roi.y - pad);
    const int rw = (std::min)(det_w - rx, roi.width + (roi.x - rx) + pad);
    const int rh = (std::min)(det_h - ry, roi.height + (roi.y - ry) + pad);
    cv::Rect exp_roi(rx, ry, rw, rh);
    if (exp_roi.area() <= 0) continue;

    cv::Mat poly_mask = cv::Mat::zeros(exp_roi.size(), CV_8U);
    std::vector<cv::Point> local_approx = approx;
    for (auto &p : local_approx) p -= exp_roi.tl();
    cv::drawContours(poly_mask, std::vector<std::vector<cv::Point>>{local_approx}, 0, cv::Scalar(255), cv::FILLED);

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(ksize, ksize));
    cv::dilate(poly_mask, poly_mask, kernel);

    std::vector<std::vector<cv::Point>> dilated;
    cv::findContours(poly_mask, dilated, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (dilated.empty()) continue;

    auto &unclipped = *std::max_element(dilated.begin(), dilated.end(), [](const auto &a, const auto &b) {
      return cv::contourArea(a) < cv::contourArea(b);
    });

    cv::RotatedRect rrect = cv::minAreaRect(unclipped);
    std::vector<cv::Point2f> pts(4);
    rrect.points(pts.data());
    for (auto &p : pts) {
      p += cv::Point2f(static_cast<float>(exp_roi.x), static_cast<float>(exp_roi.y));
    }

    std::sort(pts.begin(), pts.end(), [](const cv::Point2f &a, const cv::Point2f &b) {
      if (a.y < b.y - 1e-3f) return true;
      if (a.y > b.y + 1e-3f) return false;
      return a.x < b.x;
    });
    if (pts[0].x > pts[1].x) std::swap(pts[0], pts[1]);
    if (pts[2].x > pts[3].x) std::swap(pts[2], pts[3]);

    OcrBox obox;
    obox.score = score;
    for (const auto &p : pts) {
      obox.points.push_back({p.x * scale_w, p.y * scale_h});
    }
    boxes.push_back(obox);
  }
  return boxes;
}

OcrResult PPOCRv6Engine::Recognize(const cv::Mat &bgr_image) {
  std::lock_guard<std::mutex> lock(mutex_);
  OcrResult result;
  if (!initialized_ || bgr_image.empty()) return result;

  // Trim glibc heap after all temporary cv::Mat and Ort::Value buffers leave scope.
  ScopedHeapTrim heap_trim_guard;

  try {
    Ort::RunOptions default_run_opts{nullptr};
    Ort::RunOptions shrink_run_opts;
    shrink_run_opts.AddConfigEntry("memory.enable_memory_arena_shrinkage", "cpu:0");

    // 1. Detection preprocessing & execution in an isolated RAII scope so all detection buffers release before rec.
    cv::Mat norm_bgr = ImageFilter::NormalizeToBgr(bgr_image);
    if (norm_bgr.empty()) return result;
    std::vector<OcrBox> boxes = DetectTextBoxes(norm_bgr, shrink_run_opts, &result.det_ms);
    boxes.erase(std::remove_if(boxes.begin(), boxes.end(), [](const OcrBox &b) { return b.points.size() < 4; }),
                boxes.end());

    // Sort boxes top-to-bottom
    std::sort(boxes.begin(), boxes.end(), [](const OcrBox &a, const OcrBox &b) {
      float ay = (std::min)(a.points[0].y, a.points[1].y);
      float by = (std::min)(b.points[0].y, b.points[1].y);
      return ay < by;
    });

    // Tier 3: Box count & geometric filtering
    // If 0 boxes detected, immediately exit with success (zero text)
    if (boxes.empty()) {
      result.success = true;
      result.total_ms = result.det_ms;
      return result;
    }

    // If only 1 box detected, check if its area or dimensions are too minuscule to contain meaningful text
    if (boxes.size() == 1) {
      const auto &pts = boxes[0].points;
      float dx = pts[1].x - pts[0].x, dy = pts[1].y - pts[0].y;
      float w = std::sqrt(dx * dx + dy * dy);
      float hx = pts[3].x - pts[0].x, hy = pts[3].y - pts[0].y;
      float h = std::sqrt(hx * hx + hy * hy);
      if (w * h < 200.0f || (std::max)(w, h) < 16.0f) {
        result.success = true;
        result.total_ms = result.det_ms;
        return result;
      }
    }

    // 2. Recognition pipeline
    const char *in_names[] = {"x"};
    const char *out_names[] = {"fetch_name_0"};
    auto tr0 = std::chrono::high_resolution_clock::now();
    for (size_t bi = 0; bi < boxes.size(); ++bi) {
      const auto &b = boxes[bi];
      cv::Mat crop = GetRotateCrop(norm_bgr, b.points);
      if (crop.empty()) continue;
      if (crop.rows > crop.cols * 1.5f) {
        cv::rotate(crop, crop, cv::ROTATE_90_CLOCKWISE);
      }

      int target_h = 48;
      int target_w = (std::max)(320, int(crop.cols * (float(target_h) / crop.rows)));
      auto rec_in = RecPreprocess(crop, target_h, target_w);
      int cur_w = (int)rec_in.size() / (3 * target_h);
      std::vector<int64_t> rec_shape = {1, 3, target_h, cur_w};

      Ort::Value rec_tensor =
          Ort::Value::CreateTensor<float>(*mem_info_, rec_in.data(), rec_in.size(), rec_shape.data(), rec_shape.size());

      const bool is_last_box = (bi + 1 == boxes.size());
      auto r_out = rec_session_->Run(is_last_box ? shrink_run_opts : default_run_opts, in_names, &rec_tensor, 1,
                                     out_names, 1);
      auto type_info = r_out[0].GetTensorTypeAndShapeInfo();
      auto shape = type_info.GetShape();
      int T = static_cast<int>(shape[1]);
      int C = static_cast<int>(shape[2]);
      const float *logits = r_out[0].GetTensorData<float>();

      float conf = 0.0f;
      std::string text = CtcDecode(logits, T, C, dict_, &conf);
      if (!text.empty()) {
        // Filter out extreme low-confidence noise lines by effective character count
        if (conf < 0.35f && ImageFilter::CountEffectiveChars(text) <= 2) {
          continue;
        }

        OcrLine line;
        line.text = text;
        line.confidence = conf;
        line.box = b;
        result.lines.push_back(line);

        if (!result.full_text.empty()) result.full_text += "\n";
        result.full_text += text;
      }
    }

    auto tr1 = std::chrono::high_resolution_clock::now();
    result.rec_ms = std::chrono::duration<double, std::milli>(tr1 - tr0).count();
    result.total_ms = result.det_ms + result.rec_ms;
    result.success = true;
    return result;
  } catch (const std::exception &ex) {
    LOG_ERROR("[PPOCRv6] Exception during recognition: {}", ex.what());
    result.success = false;
    return result;
  }
}

}  // namespace ocr
}  // namespace worker
}  // namespace soeasy
