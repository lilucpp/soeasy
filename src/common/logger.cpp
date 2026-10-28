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
std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> g_console_sink;
std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> g_file_sink;
std::mutex g_logger_mutex;
std::string g_log_file_path = "logs/soeasy.log";
bool g_console_enabled = false;
bool g_sync_mode = false;
spdlog::level::level_enum g_log_level = spdlog::level::info;

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
  g_console_enabled = enable_console;
  g_log_level = level;
  EnsureLogDirectoryExists(log_file);

  std::vector<spdlog::sink_ptr> sinks;

  // Always instantiate the console sink; dynamically set level to off if disabled
  g_console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  g_console_sink->set_level(enable_console ? level : spdlog::level::off);
  sinks.push_back(g_console_sink);

  try {
    g_file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_file, max_size, max_files);
    g_file_sink->set_level(level);
    sinks.push_back(g_file_sink);
  } catch (const std::exception& e) {
    std::cerr << "[-] Failed to create rotating file sink for " << log_file
              << ": " << e.what() << "\n";
  }

  g_logger = std::make_shared<spdlog::logger>("soeasy", sinks.begin(), sinks.end());
  g_logger->set_level(level);
  g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
  g_logger->flush_on(g_sync_mode ? spdlog::level::trace : spdlog::level::warn);

  spdlog::register_logger(g_logger);
  spdlog::flush_every(std::chrono::seconds(2));
}

void Logger::Shutdown() {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  if (g_logger) {
    g_logger->flush();
    spdlog::drop_all();
    g_logger.reset();
    g_console_sink.reset();
    g_file_sink.reset();
  }
}

void Logger::SetLevel(spdlog::level::level_enum level) {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  g_log_level = level;
  if (g_logger) {
    g_logger->set_level(level);
  }
  if (g_file_sink) {
    g_file_sink->set_level(level);
  }
  if (g_console_sink) {
    g_console_sink->set_level(g_console_enabled ? level : spdlog::level::off);
  }
}

void Logger::SetLevel(const std::string& level_str) {
  SetLevel(ParseLevel(level_str));
}

spdlog::level::level_enum Logger::GetLevel() {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  return g_log_level;
}

void Logger::SetConsoleEnabled(bool enable) {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  g_console_enabled = enable;
  if (g_console_sink) {
    g_console_sink->set_level(enable ? g_log_level : spdlog::level::off);
  }
}

bool Logger::IsConsoleEnabled() {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  return g_console_enabled;
}

void Logger::SetSyncMode(bool sync_flush) {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  g_sync_mode = sync_flush;
  if (g_logger) {
    g_logger->flush_on(sync_flush ? spdlog::level::trace : spdlog::level::warn);
    if (sync_flush) {
      g_logger->flush();
    }
  }
}

bool Logger::IsSyncMode() {
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  return g_sync_mode;
}

spdlog::level::level_enum Logger::ParseLevel(const std::string& level_str) {
  std::string lower;
  lower.reserve(level_str.size());
  for (char c : level_str) lower.push_back(static_cast<char>(std::tolower(c)));

  if (lower == "trace") return spdlog::level::trace;
  if (lower == "debug") return spdlog::level::debug;
  if (lower == "info") return spdlog::level::info;
  if (lower == "warn" || lower == "warning") return spdlog::level::warn;
  if (lower == "err" || lower == "error") return spdlog::level::err;
  if (lower == "critical") return spdlog::level::critical;
  if (lower == "off") return spdlog::level::off;
  return spdlog::level::info;
}

std::string Logger::LevelToString(spdlog::level::level_enum level) {
  switch (level) {
    case spdlog::level::trace: return "trace";
    case spdlog::level::debug: return "debug";
    case spdlog::level::info: return "info";
    case spdlog::level::warn: return "warn";
    case spdlog::level::err: return "error";
    case spdlog::level::critical: return "critical";
    case spdlog::level::off: return "off";
    default: return "info";
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
