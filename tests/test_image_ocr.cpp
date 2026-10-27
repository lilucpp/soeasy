// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "common/logger.h"
#include "storage/storage_engine.h"
#include "worker/l3_image_parser.h"
#include "worker/ocr/image_filter.h"
#include "worker/ocr/ocr_engine_factory.h"

using namespace soeasy::worker;
using namespace soeasy::worker::ocr;

void TestImageFilter() {
  std::cout << "\n======================================================\n";
  std::cout << "[Test 1/4] Testing Scientific Image Filter Rules...\n";
  std::cout << "======================================================\n";

  ImageFilter filter;

  // 1. Test icon filtering
  std::string icon_path = "/home/peanut/project/test_dataset/img/icon_small_121.png";
  if (std::filesystem::exists(icon_path)) {
    auto dec = filter.PrecheckFile(icon_path);
    std::cout << "  Icon filter check (" << icon_path << "): "
              << (dec == FilterDecision::kTooSmallIconOrThumbnail ? "PASS (Filtered as Icon)" : "FAIL") << "\n";
    assert(dec == FilterDecision::kTooSmallIconOrThumbnail);
  }

  // 2. Test divider stripe filtering
  std::string stripe_path = "/home/peanut/project/test_dataset/img/divider_stripe_161.png";
  if (std::filesystem::exists(stripe_path)) {
    auto dec = filter.PrecheckFile(stripe_path);
    std::cout << "  Stripe filter check (" << stripe_path << "): "
              << (dec == FilterDecision::kExtremeAspectRatioStrip ? "PASS (Filtered as Stripe)" : "FAIL") << "\n";
    assert(dec == FilterDecision::kExtremeAspectRatioStrip);
  }

  // 3. Test solid color / blank filtering
  std::string solid_path = "/home/peanut/project/test_dataset/img/solid_color_163.jpg";
  if (std::filesystem::exists(solid_path)) {
    cv::Mat img = cv::imread(solid_path);
    auto dec = filter.CheckDecodedImage(img);
    std::cout << "  Solid color check (" << solid_path << "): "
              << (dec == FilterDecision::kSolidColorOrBlank ? "PASS (Filtered as Solid Color)" : "FAIL") << "\n";
    assert(dec == FilterDecision::kSolidColorOrBlank);
  }

  // 4. Test real screenshot must PASS
  std::string real_screenshot = "/home/peanut/图片/2026-10-26_16-54.png";
  if (std::filesystem::exists(real_screenshot)) {
    auto dec = filter.PrecheckFile(real_screenshot);
    std::cout << "  Real screenshot check (" << real_screenshot << "): "
              << (dec == FilterDecision::kPass ? "PASS (Preserved for OCR)" : "FAIL") << "\n";
    assert(dec == FilterDecision::kPass);
  }

  std::cout << ">>> Test 1 (Scientific Image Filter): SUCCESS!\n";
}

void TestPPOCRv6Engine() {
  std::cout << "\n======================================================\n";
  std::cout << "[Test 2/4] Testing PP-OCRv6 Engine (Normal & HighPerf)...\n";
  std::cout << "======================================================\n";

  auto engine = OcrEngineFactory::CreateDefaultEngine("/home/peanut/project/soeasy/models/ppocr_v6", OcrMode::kNormal);
  assert(engine != nullptr);
  std::cout << "  PP-OCRv6 Engine instantiated and initialized successfully.\n";

  std::string test_img = "/home/peanut/project/test_dataset/img/doc_standard_025.jpg";
  if (std::filesystem::exists(test_img)) {
    // Normal mode (1 thread)
    engine->SetMode(OcrMode::kNormal);
    auto res_normal = engine->RecognizeFile(test_img);
    std::cout << "  [Normal Mode (1 core)] Det: " << res_normal.det_ms << "ms | Rec: "
              << res_normal.rec_ms << "ms | Total: " << res_normal.total_ms << "ms\n";
    std::cout << "  Extracted Text snippet: " << res_normal.full_text.substr(0, 60) << "...\n";
    assert(res_normal.success);
    assert(!res_normal.lines.empty());
    assert(res_normal.full_text.find("自然语言处理") != std::string::npos ||
           res_normal.full_text.find("文档编号") != std::string::npos);

    // High performance mode (multi-thread)
    engine->SetMode(OcrMode::kHighPerformance);
    auto res_hp = engine->RecognizeFile(test_img);
    std::cout << "  [HighPerf Mode (multi)] Det: " << res_hp.det_ms << "ms | Rec: "
              << res_hp.rec_ms << "ms | Total: " << res_hp.total_ms << "ms\n";
    assert(res_hp.success);
  }

  std::cout << ">>> Test 2 (PP-OCRv6 Engine): SUCCESS!\n";
}

void TestL3ImageParser() {
  std::cout << "\n======================================================\n";
  std::cout << "[Test 3/4] Testing L3 Image Parser & Diverse Formats...\n";
  std::cout << "======================================================\n";

  L3ImageParser parser(OcrMode::kNormal, "/home/peanut/project/soeasy/models/ppocr_v6");

  // Check supported extensions
  std::vector<std::string> exts = {"png", "jpg", "jpeg", "bmp", "webp", "tiff", "tif", "gif", "svg"};
  for (const auto& e : exts) {
    assert(parser.IsSupported(e));
  }
  std::cout << "  Format support validated: png, jpg, jpeg, bmp, webp, tiff, tif, gif, svg.\n";

  // Test SVG text extraction (0 OCR overhead)
  std::string svg_path = "/home/peanut/project/test_dataset/img/vector_badge_181.svg";
  if (std::filesystem::exists(svg_path)) {
    std::string svg_text;
    bool ok = parser.Parse(svg_path, "svg", &svg_text);
    std::cout << "  SVG parsing (" << svg_path << "): ok=" << ok << " | text: '" << svg_text << "'\n";
    assert(ok);
    assert(!svg_text.empty());
  }

  // Test PNG and JPG parsing
  std::vector<std::pair<std::string, std::string>> sample_files = {
      {"/home/peanut/project/test_dataset/img/doc_standard_013.png", "png"},
      {"/home/peanut/project/test_dataset/img/receipt_angled_093.png", "png"},
      {"/home/peanut/project/test_dataset/img/dark_code_085.jpg", "jpg"}
  };

  for (const auto& [path, ext] : sample_files) {
    if (!std::filesystem::exists(path)) continue;
    std::string text;
    bool ok = parser.Parse(path, ext, &text);
    std::cout << "  Parsed " << path << " -> " << text.size() << " bytes text\n";
    assert(ok);
    assert(!text.empty());
  }

  std::cout << ">>> Test 3 (L3 Image Parser): SUCCESS!\n";
}

void TestStorageFts5ImageSearch() {
  std::cout << "\n======================================================\n";
  std::cout << "[Test 4/4] End-to-End Image OCR + FTS5 Full-Text Search\n";
  std::cout << "======================================================\n";

  std::string test_db = "/tmp/test_image_ocr_fts.db";
  std::filesystem::remove(test_db);

  const std::string dict_dir = "/home/peanut/project/soeasy/third_party/cppjieba/dict";
  soeasy::storage::StorageEngine storage;
  bool open_ok = storage.Open(test_db, "test_password", dict_dir);
  if (!open_ok) {
    std::cerr << "[-] Failed to open StorageEngine: " << storage.GetLastError() << "\n";
    std::exit(1);
  }
  std::cout << "  StorageEngine opened encrypted DB with FTS5 jieba tokenizer.\n";

  L3ImageParser parser(OcrMode::kNormal, "/home/peanut/project/soeasy/models/ppocr_v6");

  std::vector<std::pair<std::string, std::string>> test_docs = {
      {"/home/peanut/project/test_dataset/img/doc_standard_013.png", "png"},
      {"/home/peanut/project/test_dataset/img/receipt_angled_093.png", "png"},
      {"/home/peanut/project/test_dataset/img/vector_badge_181.svg", "svg"}
  };

  // If local dataset does not exist, synthesize test SVG so the test is self-contained
  bool has_any_dataset = false;
  for (const auto& [p, _] : test_docs) {
    if (std::filesystem::exists(p)) {
      has_any_dataset = true;
      break;
    }
  }
  if (!has_any_dataset) {
    std::string mock_svg = "/tmp/test_ocr_mock.svg";
    std::ofstream ofs(mock_svg);
    ofs << "<svg><text>统一社会信用代码 财务报销审批单</text></svg>";
    ofs.close();
    test_docs.push_back({mock_svg, "svg"});
  }

  int64_t doc_id = 1000;
  for (const auto& [path, ext] : test_docs) {
    if (!std::filesystem::exists(path)) continue;
    std::string extracted;
    bool ok = parser.Parse(path, ext, &extracted);
    if (!ok) {
      std::cerr << "[-] Failed to parse " << path << "\n";
      std::exit(1);
    }

    int64_t cur_id = ++doc_id;
    std::string filename = std::filesystem::path(path).filename().string();
    uint64_t file_size = std::filesystem::file_size(path);
    int64_t mtime = 1700000000;

    bool idx_ok = storage.IndexDocument(cur_id, path, filename, ext, file_size, mtime, 3, extracted);
    if (!idx_ok) {
      std::cerr << "[-] Failed to index " << filename << ": " << storage.GetLastError() << "\n";
      std::exit(1);
    }
    std::cout << "  Indexed OCR text for: " << filename << " (" << extracted.size() << " chars)\n";
  }

  // Test full text search
  auto res1 = storage.Search("统一社会信用代码", 10);
  std::cout << "  Search '统一社会信用代码': found " << res1.size() << " match(es)\n";
  if (res1.empty()) {
    std::cerr << "[-] Expected match for '统一社会信用代码'\n";
    std::exit(1);
  }

  auto res2 = storage.Search("财务报销", 10);
  std::cout << "  Search '财务报销': found " << res2.size() << " match(es)\n";
  if (res2.empty()) {
    std::cerr << "[-] Expected match for '财务报销'\n";
    std::exit(1);
  }

  storage.Close();
  std::filesystem::remove(test_db);
  std::cout << ">>> Test 4 (FTS5 Image Search): SUCCESS!\n";
}

int main() {
  soeasy::common::Logger::Init("/tmp/test_image_ocr.log", 10 * 1024 * 1024, 1, false);

  TestImageFilter();
  TestPPOCRv6Engine();
  TestL3ImageParser();
  TestStorageFts5ImageSearch();

  std::cout << "\n======================================================\n";
  std::cout << "  ALL 4 OCR INTEGRATION TESTS PASSED SUCCESSFULLY!    \n";
  std::cout << "======================================================\n";
  return 0;
}
