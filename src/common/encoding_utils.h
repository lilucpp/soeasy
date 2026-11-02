// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_COMMON_ENCODING_UTILS_H_
#define SOEASY_COMMON_ENCODING_UTILS_H_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/stat.hpp>

namespace soeasy {
namespace common {

class EncodingUtils {
 public:
  // Convert an arbitrary input string to valid UTF-8.
  // If the string is already valid UTF-8, it is returned unchanged.
  // If not, it is treated as Windows Local ANSI (e.g. GBK/CP936, CP1252) and converted to UTF-8.
  static std::string ToUtf8(const std::string& input);

  // Convert UTF-8 to wide string (std::wstring) across platforms
  static std::wstring Utf8ToWide(const std::string& utf8_str);

  // Convert wide string (std::wstring) to UTF-8 across platforms
  static std::string WideToUtf8(const std::wstring& wstr);

  // Convert UTF-8 path to the local OS native path representation required by
  // legacy C/C++ libraries that only accept const char* (such as OpenCV cv::imread).
  // On Windows: returns an 8.3 Short Path (pure ASCII) if available, or CP_ACP (GBK/ANSI),
  // ensuring third-party libraries can read directly from disk without whole-file memory buffering.
  // On POSIX: returns the UTF-8 path directly.
  static std::string ToNativeAnsiPath(const std::string& utf8_path);

#ifdef _WIN32
  // Windows-specific: Convert UTF-8 to Local ANSI (e.g. GBK/CP936 or CP1252)
  static std::string Utf8ToAnsi(const std::string& utf8_str);
#endif

  // Construct a std::filesystem::path from a UTF-8 string safely on all platforms.
  static std::filesystem::path PathFromUtf8(const std::string& utf8_path);

  // Extract a UTF-8 string from a std::filesystem::path safely on all platforms.
  static std::string PathToUtf8(const std::filesystem::path& p);

  // Cross-platform robust stat for UTF-8 path.
  // Avoids Windows ANSI stat() failure on Unicode paths (100% reliable for CJK/multilingual).
  static bool GetFileStat(const std::string& utf8_path, uint64_t* out_size, int64_t* out_mtime);

  // Safely open a streaming boost::nowide::ifstream for a UTF-8 file path across platforms.
  // Reads directly from disk on both Windows and POSIX without memory buffering.
  static boost::nowide::ifstream OpenIfstream(const std::string& utf8_path,
                                              std::ios::openmode mode = std::ios::binary);

  // Safely open a C FILE* handle for a UTF-8 file path across platforms.
  static FILE* FopenUtf8(const std::string& utf8_path, const char* mode);

  // Read file (or up to max_bytes) into a buffer (utility for small metadata or header probe).
  static bool ReadFileBinary(const std::string& utf8_path, std::string* out_data,
                             size_t max_bytes = 0);
};

}  // namespace common
}  // namespace soeasy

#endif  // SOEASY_COMMON_ENCODING_UTILS_H_
