// Copyright 2026 SoEasy Team. All rights reserved.
// Benchmark and investigation for the impact of log printing on parsing performance.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "common/logger.h"
#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#include "tests/test_helper.h"
#include "worker/l1_text_parser.h"
#include "worker/l2_office_parser.h"
#include "worker/l3_image_parser.h"

namespace fs = std::filesystem;

struct BenchResult {
  std::string name;
  double total_ms;
  double pure_parse_ms;
  double log_overhead_ms;
  double percent_overhead;
  double files_per_sec;
};

// Helper to collect test files
std::vector<std::pair<std::string, std::string>> CollectFiles(const std::string& root_dir,
                                                             const std::vector<std::string>& target_exts,
                                                             size_t max_count) {
  std::vector<std::pair<std::string, std::string>> results;
  if (!fs::exists(root_dir)) return results;

  for (const auto& entry : fs::recursive_directory_iterator(root_dir, fs::directory_options::skip_permission_denied)) {
    if (!entry.is_regular_file()) continue;
    std::string ext = entry.path().extension().string();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    for (auto& c : ext) c = std::tolower(c);

    for (const auto& target : target_exts) {
      if (ext == target) {
        try {
          if (fs::file_size(entry.path()) > 0) {
            results.push_back({entry.path().string(), ext});
            if (results.size() >= max_count) return results;
          }
        } catch (...) {}
        break;
      }
    }
  }
  return results;
}

// -------------------------------------------------------------
// Part 1: Microbenchmark of raw log overhead (10,000 operations)
// -------------------------------------------------------------
void RunMicroBenchmark() {
  std::cout << "\n======================================================\n";
  std::cout << " [1/3] Micro-benchmark: 10,000 Log Invocations Cost   \n";
  std::cout << "======================================================\n";

  const int iterations = 10000;
  const std::string dummy_file = "/path/to/test_dataset/document_spec_2026.docx";
  const std::string dummy_ext = "docx";
  const size_t dummy_bytes = 48512;
  const double dummy_cost = 1.25;

  auto test_logger = [&](const std::string& label, std::shared_ptr<spdlog::logger> logger) {
    auto t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
      logger->info("[Worker] Parse SUCCESS | file: '{}' | ext: '{}' | extracted: {} bytes | cost: {:.2f} ms",
                   dummy_file, dummy_ext, dummy_bytes, dummy_cost);
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    double cost_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double us_per_op = (cost_ms * 1000.0) / iterations;
    double ops_per_sec = (iterations / (cost_ms / 1000.0));

    std::cout << std::left << std::setw(38) << label
              << " | Total: " << std::setw(8) << std::fixed << std::setprecision(2) << cost_ms << " ms"
              << " | " << std::setw(6) << std::setprecision(2) << us_per_op << " μs/log"
              << " | " << std::setw(10) << static_cast<size_t>(ops_per_sec) << " logs/sec\n";
  };

  std::string tmp_dir = fs::temp_directory_path().string();

  // 1. Console + File (Current Default)
  {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(tmp_dir + "/bench_default.log", 10 * 1024 * 1024, 3));
    auto logger = std::make_shared<spdlog::logger>("bench_default", sinks.begin(), sinks.end());
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    test_logger("Mode 1: Console(stdout) + File (Default)", logger);
  }

  // 2. File Sink Only (Console Disabled)
  {
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(tmp_dir + "/bench_file.log", 10 * 1024 * 1024, 3);
    auto logger = std::make_shared<spdlog::logger>("bench_file", sink);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    test_logger("Mode 2: File Only (Console Disabled)", logger);
  }

  // 3. Async Logger with background worker thread
  {
    spdlog::init_thread_pool(8192, 1);
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(tmp_dir + "/bench_async.log", 10 * 1024 * 1024, 3);
    auto logger = std::make_shared<spdlog::async_logger>(
        "bench_async", sink, spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    test_logger("Mode 3: Async File Logger", logger);
  }

  // 4. Log Level WARN (Info Logs Discarded at Call Site)
  {
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(tmp_dir + "/bench_warn.log", 10 * 1024 * 1024, 3);
    auto logger = std::make_shared<spdlog::logger>("bench_warn", sink);
    logger->set_level(spdlog::level::warn);
    test_logger("Mode 4: Level WARN (Zero overhead skip)", logger);
  }
}

// -------------------------------------------------------------
// Part 2: Real Parsing on test_dataset Files with Different Log Modes
// -------------------------------------------------------------
void RunDatasetBenchmark(const std::string& dataset_name,
                        const std::vector<std::pair<std::string, std::string>>& files) {
  std::cout << "\n======================================================\n";
  std::cout << " [2/3] Dataset Parsing Test: " << dataset_name
            << " (" << files.size() << " files)\n";
  std::cout << "======================================================\n";

  if (files.empty()) {
    std::cout << "[-] No matching files found in test_dataset for " << dataset_name << "\n";
    return;
  }

  soeasy::worker::L1TextParser l1_parser;
  soeasy::worker::L2OfficeParser l2_parser;
  soeasy::worker::L3ImageParser l3_parser(soeasy::worker::ocr::OcrMode::kNormal, soeasy::test::FindModelsDir());

  auto parse_file = [&](const std::string& path, const std::string& ext, std::string* out) {
    bool is_scanned_pdf = false;
    if (l3_parser.IsSupported(ext)) {
      return l3_parser.Parse(path, ext, out);
    } else if (l2_parser.IsSupported(ext)) {
      return l2_parser.Parse(path, ext, out, &is_scanned_pdf);
    } else {
      return l1_parser.ParseFile(path, ext, out);
    }
  };

  enum class TestMode {
    kBaselineNoLog,
    kConsoleAndFile,
    kFileOnly,
    kLogLevelWarn
  };

  const std::string ds_log_file = (fs::temp_directory_path() / "dataset_run.log").string();

  auto test_run = [&](const std::string& label, TestMode mode) {
    std::shared_ptr<spdlog::logger> logger;
    if (mode == TestMode::kConsoleAndFile) {
      std::vector<spdlog::sink_ptr> sinks;
      sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
      sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(ds_log_file, 10 * 1024 * 1024, 3));
      logger = std::make_shared<spdlog::logger>("ds_console", sinks.begin(), sinks.end());
      logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    } else if (mode == TestMode::kFileOnly) {
      auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(ds_log_file, 10 * 1024 * 1024, 3);
      logger = std::make_shared<spdlog::logger>("ds_file", sink);
      logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    } else if (mode == TestMode::kLogLevelWarn) {
      auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(ds_log_file, 10 * 1024 * 1024, 3);
      logger = std::make_shared<spdlog::logger>("ds_warn", sink);
      logger->set_level(spdlog::level::warn);
    }

    size_t success_count = 0;
    size_t total_extracted = 0;
    double pure_parse_acc_ms = 0.0;

    auto start_all = std::chrono::high_resolution_clock::now();

    for (const auto& [path, ext] : files) {
      std::string text;
      auto t_p1 = std::chrono::high_resolution_clock::now();
      bool ok = parse_file(path, ext, &text);
      auto t_p2 = std::chrono::high_resolution_clock::now();

      double parse_ms = std::chrono::duration<double, std::milli>(t_p2 - t_p1).count();
      pure_parse_acc_ms += parse_ms;

      if (ok) {
        success_count++;
        total_extracted += text.size();
        if (logger) {
          logger->info("[Worker] Parse SUCCESS | file: '{}' | ext: '{}' | extracted: {} bytes | cost: {:.2f} ms",
                       path, ext, text.size(), parse_ms);
        }
      } else {
        if (logger) {
          logger->error("[Worker] Parse FAILED | file: '{}' | ext: '{}' | cost: {:.2f} ms",
                        path, ext, parse_ms);
        }
      }
    }

    auto end_all = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(end_all - start_all).count();
    double log_overhead_ms = (mode == TestMode::kBaselineNoLog) ? 0.0 : (total_ms - pure_parse_acc_ms);
    if (log_overhead_ms < 0) log_overhead_ms = 0;
    double percent_overhead = (total_ms > 0) ? (log_overhead_ms / total_ms * 100.0) : 0.0;
    double fps = (files.size() / (total_ms / 1000.0));

    std::cout << std::left << std::setw(32) << label
              << " | Total: " << std::setw(8) << std::fixed << std::setprecision(2) << total_ms << " ms"
              << " | Parse: " << std::setw(8) << pure_parse_acc_ms << " ms"
              << " | Log: " << std::setw(7) << log_overhead_ms << " ms"
              << " (" << std::setw(5) << std::setprecision(1) << percent_overhead << "%)"
              << " | Throughput: " << std::setw(7) << std::setprecision(1) << fps << " files/s\n";
  };

  test_run("1. Baseline (No logging)", TestMode::kBaselineNoLog);
  test_run("2. Console + File (Current Default)", TestMode::kConsoleAndFile);
  test_run("3. File Only (Console Disabled)", TestMode::kFileOnly);
  test_run("4. Log Level WARN (Production)", TestMode::kLogLevelWarn);
}

// -------------------------------------------------------------
// Part 3: Dual Log Overhead (Worker + Pipeline Double Logging)
// -------------------------------------------------------------
void RunDoubleLogBenchmark() {
  std::cout << "\n======================================================\n";
  std::cout << " [3/3] End-to-End Simulation: Dual Logging (Worker + Pipeline)\n";
  std::cout << "======================================================\n";

  const int files_count = 2000;
  std::cout << "Simulating dual logging for " << files_count << " files...\n";

  const std::string bench_dual_log = (fs::temp_directory_path() / "bench_dual.log").string();
  auto test_dual = [&](const std::string& label, bool enable_console, spdlog::level::level_enum level) {
    std::vector<spdlog::sink_ptr> sinks;
    if (enable_console) {
      sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(bench_dual_log, 10 * 1024 * 1024, 3));
    auto logger = std::make_shared<spdlog::logger>("dual_logger", sinks.begin(), sinks.end());
    logger->set_level(level);

    auto t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < files_count; ++i) {
      // 1. Worker logging
      logger->info("[Worker] Parse SUCCESS | file: '/path/test_{}.md' | ext: 'md' | extracted: 1024 bytes | cost: 0.15 ms", i);
      // 2. Main pipeline logging
      logger->info("[Parser] SUCCESS parsed & indexed '/path/test_{}.md' (ext: 'md', size: 1024 B, extracted: 1024 B) | parse: 0.15 ms, total: 0.40 ms", i);
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double us_per_file = (total_ms * 1000.0) / files_count;

    std::cout << std::left << std::setw(38) << label
              << " | Total: " << std::setw(8) << std::fixed << std::setprecision(2) << total_ms << " ms"
              << " | " << std::setw(6) << std::setprecision(2) << us_per_file << " μs/file"
              << " | " << std::setw(10) << static_cast<size_t>(files_count / (total_ms / 1000.0)) << " files/sec\n";
  };

  test_dual("Console + File (Current Default)", true, spdlog::level::info);
  test_dual("File Only (Console Disabled)", false, spdlog::level::info);
  test_dual("Level WARN (Console Disabled)", false, spdlog::level::warn);
}

int main() {
  const std::string ds_root = soeasy::test::FindTestDatasetDir();

  std::cout << "######################################################\n";
  std::cout << "  SoEasy Parser Log Printing Performance Investigation\n";
  std::cout << "  Dataset: " << ds_root << "\n";
  std::cout << "######################################################\n";

  // 1. Micro-benchmark
  RunMicroBenchmark();

  // 2. Dataset Real Files Benchmark

  // Batch A: Pure Text Files (Markdown, HTML, Code) - Fast Parsing
  auto text_files = CollectFiles(ds_root, {"md", "txt", "html", "cpp", "hs"}, 500);
  RunDatasetBenchmark("Fast Text Batch (L1 Parser)", text_files);

  // Batch B: Office Documents (DOCX, PPTX, PDF) - Moderate Parsing
  auto office_files = CollectFiles(ds_root, {"docx", "pptx", "pdf"}, 80);
  RunDatasetBenchmark("Office Documents Batch (L2 Parser)", office_files);

  // Batch C: Images & Vector (PNG, JPG, SVG) - OCR & XML Parsing
  auto image_files = CollectFiles(ds_root, {"png", "jpg", "svg"}, 50);
  RunDatasetBenchmark("Images & Vector Batch (L3 Parser)", image_files);

  // 3. Double Logging IPC Simulation
  RunDoubleLogBenchmark();

  std::cout << "\n======================================================\n";
  std::cout << "  INVESTIGATION COMPLETED SUCCESSFULLY                \n";
  std::cout << "======================================================\n";
  return 0;
}
