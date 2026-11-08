// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

#include "common/logger.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"
#include "worker/l3_image_parser.h"
#include "worker/ocr/image_filter.h"
#include "worker/ocr/ocr_engine_factory.h"

using namespace soeasy::worker;
using namespace soeasy::worker::ocr;

struct SyntheticDataset {
  std::string dir;
  std::string icon_path;
  std::string stripe_path;
  std::string solid_path;
  std::string doc_png_path;
  std::string doc_jpg_path;
  std::string svg_path;
  std::string noise_bmp_path;

  SyntheticDataset() {
    auto tmp = std::filesystem::temp_directory_path();
    dir = (tmp / ("soeasy_synthetic_ocr_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()))).string();
    std::filesystem::create_directories(dir);

    icon_path = (std::filesystem::path(dir) / "icon_16x16.png").string();
    stripe_path = (std::filesystem::path(dir) / "stripe_2x500.png").string();
    solid_path = (std::filesystem::path(dir) / "solid_white.jpg").string();
    doc_png_path = (std::filesystem::path(dir) / "doc_standard.png").string();
    doc_jpg_path = (std::filesystem::path(dir) / "doc_standard.jpg").string();
    svg_path = (std::filesystem::path(dir) / "vector_badge.svg").string();

    // 1. Icon (16x16)
    cv::Mat icon(16, 16, CV_8UC3, cv::Scalar(128, 128, 128));
    cv::imwrite(icon_path, icon);

    // 2. Stripe (2x500)
    cv::Mat stripe(2, 500, CV_8UC3, cv::Scalar(64, 64, 64));
    cv::imwrite(stripe_path, stripe);

    // 3. Solid color (300x300 white, larger than min_side 120 to test stddev blank filter)
    cv::Mat solid(300, 300, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::imwrite(solid_path, solid);

    // 4. Document image with text (600x800 white background with black text)
    cv::Mat doc(600, 800, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::putText(doc, "SoEasy AI OCR Engine 2026", cv::Point(50, 120), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                cv::Scalar(0, 0, 0), 2);
    cv::putText(doc, "Natural Language Processing", cv::Point(50, 240), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                cv::Scalar(0, 0, 0), 2);
    cv::imwrite(doc_png_path, doc);
    cv::imwrite(doc_jpg_path, doc);

    // 5. SVG
    std::ofstream ofs(svg_path);
    ofs << "<svg xmlns=\"http://www.w3.org/2000/svg\"><text x=\"20\" y=\"35\">统一社会信用代码 财务报销审批单</text></svg>";
    ofs.close();

    // 6. Single character BMP (simulates noise image with only 1 effective char)
    noise_bmp_path = (std::filesystem::path(dir) / "single_char.bmp").string();
    cv::Mat noise(300, 300, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::putText(noise, "A", cv::Point(100, 200), cv::FONT_HERSHEY_SIMPLEX, 3.0, cv::Scalar(0, 0, 0), 4);
    cv::imwrite(noise_bmp_path, noise);
  }

  ~SyntheticDataset() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

void TestImageFilter() {
  std::cout << "\n======================================================\n";
  std::cout << "[Test 1/4] Testing Scientific Image Filter Rules...\n";
  std::cout << "======================================================\n";

  ImageFilter filter;
  SyntheticDataset syn;
  std::string img_dataset = soeasy::test::FindTestDatasetDir() + "/img";

  // 1. Test icon filtering
  std::string icon_path = img_dataset + "/icon_small_121.png";
  if (!std::filesystem::exists(icon_path)) {
    icon_path = syn.icon_path;
  }
  auto dec_icon = filter.PrecheckFile(icon_path);
  std::cout << "  Icon filter check (" << icon_path
            << "): " << (dec_icon == FilterDecision::kTooSmallIconOrThumbnail ? "PASS (Filtered as Icon)" : "FAIL")
            << "\n";
  assert(dec_icon == FilterDecision::kTooSmallIconOrThumbnail);

  // 2. Test divider stripe filtering
  std::string stripe_path = img_dataset + "/divider_stripe_161.png";
  if (!std::filesystem::exists(stripe_path)) {
    stripe_path = syn.stripe_path;
  }
  auto dec_stripe = filter.PrecheckFile(stripe_path);
  std::cout << "  Stripe filter check (" << stripe_path
            << "): " << (dec_stripe == FilterDecision::kExtremeAspectRatioStrip ? "PASS (Filtered as Stripe)" : "FAIL")
            << "\n";
  assert(dec_stripe == FilterDecision::kExtremeAspectRatioStrip);

  // 3. Test solid color / blank filtering
  std::string solid_path = img_dataset + "/solid_color_163.jpg";
  if (!std::filesystem::exists(solid_path)) {
    solid_path = syn.solid_path;
  }
  cv::Mat img = cv::imread(solid_path);
  auto dec_solid = filter.CheckDecodedImage(img);
  std::cout << "  Solid color check (" << solid_path
            << "): " << (dec_solid == FilterDecision::kSolidColorOrBlank ? "PASS (Filtered as Solid Color)" : "FAIL")
            << "\n";
  assert(dec_solid == FilterDecision::kSolidColorOrBlank);

  // 4. Test real document/screenshot must PASS
  std::string real_screenshot = img_dataset + "/doc_standard_013.png";
  if (!std::filesystem::exists(real_screenshot)) {
    real_screenshot = syn.doc_png_path;
  }
  auto dec_real = filter.PrecheckFile(real_screenshot);
  std::cout << "  Real screenshot check (" << real_screenshot
            << "): " << (dec_real == FilterDecision::kPass ? "PASS (Preserved for OCR)" : "FAIL") << "\n";
  assert(dec_real == FilterDecision::kPass);

  // 5. Test path precheck for cache & thumbnails
  assert(ImageFilter::IsCacheOrThumbnailPath("/tmp/cache/WeAppIcon/123.jpg"));
  assert(ImageFilter::IsCacheOrThumbnailPath("/tmp/msg/Thumb/574_1788_thumb.jpg"));
  assert(!ImageFilter::IsCacheOrThumbnailPath("/tmp/work/invoice_2026.png"));
  std::cout << "  Path-based cache & thumbnail filter check: PASS\n";

  // 6. Test CountEffectiveChars
  assert(ImageFilter::CountEffectiveChars("福") == 1);
  assert(ImageFilter::CountEffectiveChars("哈哈哈") == 3);
  assert(ImageFilter::CountEffectiveChars("3.0") == 2);
  assert(ImageFilter::CountEffectiveChars("《走线》") == 2);
  assert(ImageFilter::CountEffectiveChars("辽宁人社") == 4);
  assert(ImageFilter::CountEffectiveChars("统一社会信用代码") == 8);
  std::cout << "  Effective character count validation: PASS\n";

  // 7. Test ProbeBufferDimensions (in-memory zero-allocation dimension probe)
  std::string probe_png = std::filesystem::exists(img_dataset + "/doc_standard_013.png")
                              ? (img_dataset + "/doc_standard_013.png")
                              : syn.doc_png_path;
  std::ifstream f_png(probe_png, std::ios::binary);
  if (f_png.is_open()) {
    std::vector<uint8_t> png_buf((std::istreambuf_iterator<char>(f_png)), std::istreambuf_iterator<char>());
    auto dim = ImageFilter::ProbeBufferDimensions(png_buf.data(), png_buf.size());
    assert(dim.valid && dim.width > 0 && dim.height > 0);
    std::cout << "  ProbeBufferDimensions (PNG: " << dim.width << "x" << dim.height << "): PASS\n";
  }

  std::string probe_jpg = std::filesystem::exists(img_dataset + "/doc_standard_003.jpg")
                              ? (img_dataset + "/doc_standard_003.jpg")
                              : syn.doc_jpg_path;
  std::ifstream f_jpg(probe_jpg, std::ios::binary);
  if (f_jpg.is_open()) {
    std::vector<uint8_t> jpg_buf((std::istreambuf_iterator<char>(f_jpg)), std::istreambuf_iterator<char>());
    auto dim = ImageFilter::ProbeBufferDimensions(jpg_buf.data(), jpg_buf.size());
    assert(dim.valid && dim.width > 0 && dim.height > 0);
    std::cout << "  ProbeBufferDimensions (JPEG: " << dim.width << "x" << dim.height << "): PASS\n";
  }

  std::cout << ">>> Test 1 (Scientific Image Filter): SUCCESS!\n";
}

void TestPPOCRv6Engine() {
  std::cout << "\n======================================================\n";
  std::cout << "[Test 2/4] Testing PP-OCRv6 Engine (Normal & HighPerf)...\n";
  std::cout << "======================================================\n";

  auto engine = OcrEngineFactory::CreateDefaultEngine(soeasy::test::FindModelsDir(), OcrMode::kNormal);
  assert(engine != nullptr);
  std::cout << "  PP-OCRv6 Engine instantiated and initialized successfully.\n";

  SyntheticDataset syn;
  std::string img_dataset = soeasy::test::FindTestDatasetDir() + "/img";
  std::string test_img = img_dataset + "/doc_standard_025.jpg";
  bool is_synthetic = false;
  if (!std::filesystem::exists(test_img)) {
    test_img = syn.doc_jpg_path;
    is_synthetic = true;
  }

  // Normal mode (1 thread)
  engine->SetMode(OcrMode::kNormal);
  auto res_normal = engine->RecognizeFile(test_img);
  std::cout << "  [Normal Mode (1 core)] Det: " << res_normal.det_ms << "ms | Rec: " << res_normal.rec_ms
            << "ms | Total: " << res_normal.total_ms << "ms\n";
  std::cout << "  Extracted Text snippet: " << res_normal.full_text.substr(0, 60) << "...\n";
  assert(res_normal.success);
  assert(!res_normal.lines.empty());
  if (is_synthetic) {
    assert(res_normal.full_text.find("SoEasy") != std::string::npos ||
           res_normal.full_text.find("Engine") != std::string::npos ||
           res_normal.full_text.find("Language") != std::string::npos);
  } else {
    assert(res_normal.full_text.find("自然语言处理") != std::string::npos ||
           res_normal.full_text.find("文档编号") != std::string::npos);
  }

  // High performance mode (multi-thread)
  engine->SetMode(OcrMode::kHighPerformance);
  auto res_hp = engine->RecognizeFile(test_img);
  std::cout << "  [HighPerf Mode (multi)] Det: " << res_hp.det_ms << "ms | Rec: " << res_hp.rec_ms
            << "ms | Total: " << res_hp.total_ms << "ms\n";
  assert(res_hp.success);

  std::cout << ">>> Test 2 (PP-OCRv6 Engine): SUCCESS!\n";
}

void TestL3ImageParser() {
  std::cout << "\n======================================================\n";
  std::cout << "[Test 3/4] Testing L3 Image Parser & Diverse Formats...\n";
  std::cout << "======================================================\n";

  L3ImageParser parser(OcrMode::kNormal, soeasy::test::FindModelsDir());

  // Check supported extensions
  std::vector<std::string> exts = {"png", "jpg", "jpeg", "bmp", "webp", "tiff", "tif", "gif", "svg"};
  for (const auto &e : exts) {
    bool supported = parser.IsSupported(e);
    (void)supported;
    assert(supported);
  }
  std::cout << "  Format support validated: png, jpg, jpeg, bmp, webp, tiff, tif, gif, svg.\n";

  SyntheticDataset syn;
  std::string img_dataset = soeasy::test::FindTestDatasetDir() + "/img";

  // Test SVG text extraction (0 OCR overhead)
  std::string svg_path = img_dataset + "/vector_badge_181.svg";
  if (!std::filesystem::exists(svg_path)) {
    svg_path = syn.svg_path;
  }
  std::string svg_text;
  bool ok = parser.Parse(svg_path, "svg", &svg_text);
  std::cout << "  SVG parsing (" << svg_path << "): ok=" << ok << " | text: '" << svg_text << "'\n";
  assert(ok);
  assert(!svg_text.empty());

  // Test PNG and JPG parsing
  std::vector<std::pair<std::string, std::string>> sample_files = {{img_dataset + "/doc_standard_013.png", "png"},
                                                                   {img_dataset + "/receipt_angled_093.png", "png"},
                                                                   {img_dataset + "/dark_code_085.jpg", "jpg"}};
  bool tested_any_image = false;
  for (const auto &[path, ext] : sample_files) {
    if (!std::filesystem::exists(path)) continue;
    std::string text;
    bool ok_parse = parser.Parse(path, ext, &text);
    (void)ok_parse;
    std::cout << "  Parsed " << path << " -> " << text.size() << " bytes text\n";
    assert(ok_parse);
    assert(!text.empty());
    tested_any_image = true;
  }
  if (!tested_any_image) {
    std::string text;
    bool ok_syn = parser.Parse(syn.doc_png_path, "png", &text);
    (void)ok_syn;
    std::cout << "  Parsed synthetic doc PNG -> " << text.size() << " bytes text\n";
    assert(ok_syn);
    assert(!text.empty());
  }

  // Test noise image with only 1 char (simulating noise image with min_chars filter)
  std::string noise_bmp = syn.noise_bmp_path;
  std::string noise_text;
  parser.set_min_effective_chars(4);
  bool ok_noise = parser.Parse(noise_bmp, "bmp", &noise_text);
  (void)ok_noise;
  std::cout << "  Noise image check (" << noise_bmp << ") with min_chars=4: text length=" << noise_text.size()
            << " (Expected 0/skipped)\n";
  assert(ok_noise);
  assert(noise_text.empty());

  // When threshold is 0, text is preserved
  parser.set_min_effective_chars(0);
  parser.Parse(noise_bmp, "bmp", &noise_text);
  std::cout << "  Noise image check (" << noise_bmp << ") with min_chars=0: text='" << noise_text << "'\n";
  assert(!noise_text.empty());

  // Test OCR Engine Lazy Loading and Idle Unload / Reload
  {
    std::cout << "  Testing OCR Lazy Loading & Idle Eviction...\n";
    L3ImageParser lazy_parser(OcrMode::kNormal, soeasy::test::FindModelsDir());
    assert(!lazy_parser.is_ocr_loaded());
    std::cout << "    • Lazy state before parse: is_ocr_loaded = false (0 MB overhead)\n";

    std::string sample_img = img_dataset + "/doc_standard_013.png";
    if (std::filesystem::exists(sample_img)) {
      std::string out_text;
      lazy_parser.Parse(sample_img, "png", &out_text);
      assert(lazy_parser.is_ocr_loaded());
      std::cout << "    • After first image task: is_ocr_loaded = true (dynamically loaded)\n";

      // Test idle unload
      lazy_parser.CheckIdleUnload(0.0);
      assert(!lazy_parser.is_ocr_loaded());
      std::cout << "    • After CheckIdleUnload(0.0): is_ocr_loaded = false (RAM reclaimed)\n";

      // Test seamless reload on subsequent task
      lazy_parser.Parse(sample_img, "png", &out_text);
      assert(lazy_parser.is_ocr_loaded());
      std::cout << "    • After subsequent image task: is_ocr_loaded = true (transparently reloaded)\n";
    }
  }

  std::cout << ">>> Test 3 (L3 Image Parser): SUCCESS!\n";
}

void TestStorageFts5ImageSearch() {
  std::cout << "\n======================================================\n";
  std::cout << "[Test 4/4] End-to-End Image OCR + FTS5 Full-Text Search\n";
  std::cout << "======================================================\n";

  std::string test_db = (std::filesystem::temp_directory_path() /
                         ("test_image_ocr_fts_" + std::to_string(getpid()) + "_" +
                          std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".db")).string();
  std::filesystem::remove(test_db);

  const std::string dict_dir = soeasy::test::FindDictDir();
  soeasy::storage::StorageEngine storage;
  bool open_ok = storage.Open(test_db, "test_password", dict_dir);
  if (!open_ok) {
    std::cerr << "[-] Failed to open StorageEngine: " << storage.GetLastError() << "\n";
    std::exit(1);
  }
  std::cout << "  StorageEngine opened encrypted DB with FTS5 jieba tokenizer.\n";

  L3ImageParser parser(OcrMode::kNormal, soeasy::test::FindModelsDir());

  std::string img_dataset = soeasy::test::FindTestDatasetDir() + "/img";
  std::vector<std::pair<std::string, std::string>> test_docs = {{img_dataset + "/doc_standard_013.png", "png"},
                                                                {img_dataset + "/receipt_angled_093.png", "png"},
                                                                {img_dataset + "/vector_badge_181.svg", "svg"}};

  // If local dataset does not exist, synthesize test SVG so the test is self-contained
  bool has_any_dataset = false;
  for (const auto &[p, _] : test_docs) {
    if (std::filesystem::exists(p)) {
      has_any_dataset = true;
      break;
    }
  }
  if (!has_any_dataset) {
    std::string mock_svg = (std::filesystem::temp_directory_path() / "test_ocr_mock.svg").string();
    std::ofstream ofs(mock_svg);
    ofs << "<svg><text>统一社会信用代码 财务报销审批单</text></svg>";
    ofs.close();
    test_docs.push_back({mock_svg, "svg"});
  }

  int64_t doc_id = 1000;
  for (const auto &[path, ext] : test_docs) {
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
  std::string log_file = (std::filesystem::temp_directory_path() / "test_image_ocr.log").string();
  soeasy::common::Logger::Init(log_file, 10 * 1024 * 1024, 1, false);

  TestImageFilter();
  TestPPOCRv6Engine();
  TestL3ImageParser();
  TestStorageFts5ImageSearch();

  std::cout << "\n======================================================\n";
  std::cout << "  ALL 4 OCR INTEGRATION TESTS PASSED SUCCESSFULLY!    \n";
  std::cout << "======================================================\n";
  return 0;
}
