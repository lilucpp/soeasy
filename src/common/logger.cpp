#include "common/logger.h"

#include <filesystem>
#include <iostream>
#include <mutex>
#include <vector>

#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

namespace soeasy {
namespace common {

namespace {

std::shared_ptr<spdlog::logger> g_logger;
std::mutex g_logger_mutex;
std::string g_log_file_path = "logs/soeasy.log";

}  // namespace

void Logger::EnsureLogDirectoryExists(const std::string& file_path) {
  try {
    std::filesystem::path p(file_path);
    if (p.has_parent_path()) {
      std::filesystem::create_directories(p.parent_path());
    }
  } catch (const std::exception& e) {
    std::cerr << "[-] Failed to create log directory: " << e.what() << "\n";
  }
}

void Logger::Init(const std::string& log_file,
                  size_t max_size,
                  size_t max_files,
                  bool enable_console,
                  spdlog::level::level_enum level) {
  std::lock_guard<std::mutex> lock(g_logger_mutex);

  // If already initialized, flush and drop old logger
  if (g_logger) {
    g_logger->flush();
    spdlog::drop("soeasy");
    g_logger.reset();
  }

  g_log_file_path = log_file;
  EnsureLogDirectoryExists(log_file);

  std::vector<spdlog::sink_ptr> sinks;

  if (enable_console) {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(level);
    sinks.push_back(console_sink);
  }

  try {
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_file, max_size, max_files);
    file_sink->set_level(level);
    sinks.push_back(file_sink);
  } catch (const std::exception& e) {
    std::cerr << "[-] Failed to create rotating file sink for " << log_file
              << ": " << e.what() << "\n";
  }

  g_logger = std::make_shared<spdlog::logger>("soeasy", sinks.begin(), sinks.end());
  g_logger->set_level(level);
  g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
  g_logger->flush_on(spdlog::level::warn);

  spdlog::register_logger(g_logger);
  spdlog::flush_every(std::chrono::seconds(2));
}

void Logger::Shutdown() {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  if (g_logger) {
    g_logger->flush();
    spdlog::drop_all();
    g_logger.reset();
  }
}

std::shared_ptr<spdlog::logger> Logger::Get() {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  if (!g_logger) {
    // Lazy default initialization
    g_log_file_path = "logs/soeasy.log";
    EnsureLogDirectoryExists(g_log_file_path);

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    try {
      sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          g_log_file_path, 10 * 1024 * 1024, 3));
    } catch (...) {}

    g_logger = std::make_shared<spdlog::logger>("soeasy", sinks.begin(), sinks.end());
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    g_logger->flush_on(spdlog::level::warn);
    spdlog::register_logger(g_logger);
  }
  return g_logger;
}

std::string Logger::GetLogFilePath() {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  return g_log_file_path;
}

}  // namespace common
}  // namespace soeasy
