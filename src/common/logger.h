#ifndef SOEASY_SRC_COMMON_LOGGER_H_
#define SOEASY_SRC_COMMON_LOGGER_H_

#include <memory>
#include <string>

#include "spdlog/spdlog.h"

namespace soeasy {
namespace common {

class Logger {
 public:
  /**
   * @brief Initialize spdlog rotating file logger and optional console sink.
   *
   * @param log_file Path to log file (default: logs/soeasy.log)
   * @param max_size Maximum size of each log file in bytes (default: 10MB)
   * @param max_files Number of rolling files (default: 3)
   * @param enable_console Whether to mirror logs to stdout/stderr
   * @param level Default log level (default: info)
   */
  static void Init(const std::string& log_file = "logs/soeasy.log",
                   size_t max_size = 10 * 1024 * 1024,
                   size_t max_files = 3,
                   bool enable_console = true,
                   spdlog::level::level_enum level = spdlog::level::info);

  /**
   * @brief Flush and shutdown spdlog registry.
   */
  static void Shutdown();

  /**
   * @brief Get shared logger instance (lazy initializes defaults if needed).
   */
  static std::shared_ptr<spdlog::logger> Get();

  /**
   * @brief Get active log file path.
   */
  static std::string GetLogFilePath();

 private:
  static void EnsureLogDirectoryExists(const std::string& file_path);
};

}  // namespace common
}  // namespace soeasy

// Standard log macros
#define LOG_TRACE(...)    ::soeasy::common::Logger::Get()->trace(__VA_ARGS__)
#define LOG_DEBUG(...)    ::soeasy::common::Logger::Get()->debug(__VA_ARGS__)
#define LOG_INFO(...)     ::soeasy::common::Logger::Get()->info(__VA_ARGS__)
#define LOG_WARN(...)     ::soeasy::common::Logger::Get()->warn(__VA_ARGS__)
#define LOG_ERROR(...)    ::soeasy::common::Logger::Get()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) ::soeasy::common::Logger::Get()->critical(__VA_ARGS__)

#endif  // SOEASY_SRC_COMMON_LOGGER_H_
