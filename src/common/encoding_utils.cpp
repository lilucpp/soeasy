// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "common/encoding_utils.h"

#include <sys/stat.h>
#include <algorithm>
#include <vector>

#include "utf8.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace soeasy {
namespace common {

std::string EncodingUtils::ToUtf8(const std::string& input) {
  if (input.empty()) {
    return "";
  }

  // 1. If input is already valid UTF-8, return as-is
  if (utf8::is_valid(input.begin(), input.end())) {
    return input;
  }

#ifdef _WIN32
  // 2. On Windows: treat as current ANSI Code Page (CP_ACP, e.g. GBK 936, Windows-1252)
  int wlen = MultiByteToWideChar(CP_ACP, 0, input.c_str(), static_cast<int>(input.size()),
                                 nullptr, 0);
  if (wlen > 0) {
    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, input.c_str(), static_cast<int>(input.size()),
                        &wstr[0], wlen);
    return WideToUtf8(wstr);
  }
#endif

  // 3. Fallback: replace invalid characters with standard replacement mark
  std::string sanitized;
  sanitized.reserve(input.size());
  utf8::replace_invalid(input.begin(), input.end(), std::back_inserter(sanitized), ' ');
  return sanitized;
}

std::wstring EncodingUtils::Utf8ToWide(const std::string& utf8_str) {
  if (utf8_str.empty()) {
    return L"";
  }
  try {
    return boost::nowide::widen(utf8_str);
  } catch (...) {
    return L"";
  }
}

std::string EncodingUtils::WideToUtf8(const std::wstring& wstr) {
  if (wstr.empty()) {
    return "";
  }
  try {
    return boost::nowide::narrow(wstr);
  } catch (...) {
    return "";
  }
}

std::string EncodingUtils::ToNativeAnsiPath(const std::string& utf8_path) {
#ifdef _WIN32
  if (utf8_path.empty()) {
    return "";
  }
  std::wstring wpath = Utf8ToWide(utf8_path);
  if (wpath.empty()) {
    return utf8_path;
  }
  // Standardize slashes to backslashes for Win32 API compatibility
  std::replace(wpath.begin(), wpath.end(), L'/', L'\\');

  // 1. Try 8.3 Short Path first (pure ASCII, 100% immune to ANSI/locale bugs in legacy C/C++ libraries)
  DWORD short_len = GetShortPathNameW(wpath.c_str(), nullptr, 0);
  if (short_len > 0) {
    std::wstring short_wpath(short_len, L'\0');
    if (GetShortPathNameW(wpath.c_str(), &short_wpath[0], short_len) > 0) {
      short_wpath.resize(short_len - 1);
      int len = WideCharToMultiByte(CP_ACP, 0, short_wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (len > 1) {
        std::string ansi(len - 1, '\0');
        WideCharToMultiByte(CP_ACP, 0, short_wpath.c_str(), -1, &ansi[0], len, nullptr, nullptr);
        return ansi;
      }
    }
  }

  // 2. Fallback to CP_ACP (GBK on Chinese Windows, Windows-1252 on Western)
  int len = WideCharToMultiByte(CP_ACP, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (len > 1) {
    std::string ansi(len - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, wpath.c_str(), -1, &ansi[0], len, nullptr, nullptr);
    return ansi;
  }
  return utf8_path;
#else
  return utf8_path;
#endif
}

#ifdef _WIN32
std::string EncodingUtils::Utf8ToAnsi(const std::string& utf8_str) {
  return ToNativeAnsiPath(utf8_str);
}
#endif

std::filesystem::path EncodingUtils::PathFromUtf8(const std::string& utf8_path) {
#ifdef _WIN32
  return std::filesystem::path(Utf8ToWide(utf8_path));
#else
  return std::filesystem::path(utf8_path);
#endif
}

std::string EncodingUtils::PathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
  return WideToUtf8(p.wstring());
#else
  return p.string();
#endif
}

bool EncodingUtils::GetFileStat(const std::string& utf8_path, uint64_t* out_size,
                                int64_t* out_mtime) {
#ifdef _WIN32
  std::wstring wpath = Utf8ToWide(utf8_path);
  if (wpath.empty()) {
    return false;
  }
  struct _stat64 st;
  if (_wstat64(wpath.c_str(), &st) == 0) {
    if (out_size) *out_size = static_cast<uint64_t>(st.st_size);
    if (out_mtime) *out_mtime = static_cast<int64_t>(st.st_mtime);
    return true;
  }

  // Fallback to GetFileAttributesExW (useful for special files/locked files)
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
    if (out_size) {
      ULARGE_INTEGER size;
      size.HighPart = fad.nFileSizeHigh;
      size.LowPart = fad.nFileSizeLow;
      *out_size = size.QuadPart;
    }
    if (out_mtime) {
      ULARGE_INTEGER ull;
      ull.LowPart = fad.ftLastWriteTime.dwLowDateTime;
      ull.HighPart = fad.ftLastWriteTime.dwHighDateTime;
      *out_mtime = static_cast<int64_t>((ull.QuadPart - 116444736000000000ULL) / 10000000ULL);
    }
    return true;
  }
  return false;
#else
  struct stat st;
  if (stat(utf8_path.c_str(), &st) == 0) {
    if (out_size) *out_size = static_cast<uint64_t>(st.st_size);
    if (out_mtime) *out_mtime = static_cast<int64_t>(st.st_mtime);
    return true;
  }
  return false;
#endif
}

boost::nowide::ifstream EncodingUtils::OpenIfstream(const std::string& utf8_path,
                                                   std::ios::openmode mode) {
  return boost::nowide::ifstream(utf8_path, mode);
}

FILE* EncodingUtils::FopenUtf8(const std::string& utf8_path, const char* mode) {
  return boost::nowide::fopen(utf8_path.c_str(), mode);
}

bool EncodingUtils::ReadFileBinary(const std::string& utf8_path,
                                   std::string* out_data, size_t max_bytes) {
  if (!out_data) {
    return false;
  }
  out_data->clear();

  auto ifs = OpenIfstream(utf8_path, std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }

  if (max_bytes > 0) {
    out_data->resize(max_bytes);
    ifs.read(&(*out_data)[0], max_bytes);
    out_data->resize(ifs.gcount());
  } else {
    out_data->assign((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
  }
  return true;
}

}  // namespace common
}  // namespace soeasy
