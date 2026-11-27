#include "service/worker_process_utils.h"

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <limits.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace soeasy {
namespace service {

void SubprocessDeleter::operator()(subprocess_s *proc) const noexcept {
  if (!proc) {
    return;
  }
#if defined(_WIN32)
  if (proc->hProcess) {
    if (subprocess_alive(proc)) {
      subprocess_terminate(proc);
    }
    int exit_code = 0;
    subprocess_join(proc, &exit_code);
  }
#else
  if (proc->child > 0 || proc->alive) {
    if (subprocess_alive(proc)) {
      subprocess_terminate(proc);
    }
    int exit_code = 0;
    subprocess_join(proc, &exit_code);
  }
#endif
  subprocess_destroy(proc);
  delete proc;
}

std::vector<const char *> ToArgv(const std::vector<std::string> &args) {
  std::vector<const char *> cmd;
  cmd.reserve(args.size() + 1);
  for (const auto &arg : args) {
    cmd.push_back(arg.c_str());
  }
  cmd.push_back(nullptr);
  return cmd;
}

namespace {

std::filesystem::path PathFromUtf8(const std::string &utf8) {
#ifdef _WIN32
  return std::filesystem::u8path(utf8);
#else
  return std::filesystem::path(utf8);
#endif
}

std::string PathToUtf8(const std::filesystem::path &path) {
#ifdef _WIN32
  return path.u8string();
#else
  return path.string();
#endif
}

std::string PathToGenericUtf8(const std::filesystem::path &path) {
#ifdef _WIN32
  return path.generic_u8string();
#else
  return path.generic_string();
#endif
}

}  // namespace

std::string ResolveWorkerBinary(const std::string &hint_path) {
  std::error_code ec;
  if (!hint_path.empty()) {
    std::filesystem::path hint = PathFromUtf8(hint_path);
    if (std::filesystem::exists(hint, ec)) {
      std::filesystem::path resolved = std::filesystem::canonical(hint, ec);
      if (!ec) {
        return PathToUtf8(resolved);
      }
    }
  }

#ifdef _WIN32
  std::vector<wchar_t> exe_buf(MAX_PATH);
  while (exe_buf.size() <= 32768) {
    DWORD len = GetModuleFileNameW(nullptr, exe_buf.data(), static_cast<DWORD>(exe_buf.size()));
    if (len == 0) {
      break;
    }
    if (len < exe_buf.size()) {
      std::filesystem::path dir = std::filesystem::path(exe_buf.data()).parent_path();
      for (const auto &target : {dir / L"soeasy-worker.exe", dir / L"bin" / L"soeasy-worker.exe"}) {
        if (std::filesystem::exists(target, ec)) {
          std::filesystem::path resolved = std::filesystem::canonical(target, ec);
          if (!ec) {
            return PathToUtf8(resolved);
          }
        }
      }
      break;
    }
    exe_buf.resize(exe_buf.size() * 2);
  }
#elif defined(__linux__)
  char exe_buf[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
  if (len > 0) {
    exe_buf[len] = '\0';
    std::filesystem::path dir = std::filesystem::path(exe_buf).parent_path();
    for (const auto &target : {dir / "soeasy-worker", dir / "bin" / "soeasy-worker"}) {
      if (std::filesystem::exists(target, ec)) {
        std::filesystem::path resolved = std::filesystem::canonical(target, ec);
        if (!ec) {
          return PathToUtf8(resolved);
        }
      }
    }
  }
#endif

  return "";
}

size_t GetProcessRssBytes(WorkerPid pid) {
  if (pid <= 0) {
    return 0;
  }
#ifdef _WIN32
  HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(pid));
  if (!hProcess) return 0;
  PROCESS_MEMORY_COUNTERS pmc;
  size_t bytes = 0;
  if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
    bytes = pmc.WorkingSetSize;
  }
  CloseHandle(hProcess);
  return bytes;
#else
  std::string statm_path = "/proc/" + std::to_string(pid) + "/statm";
  std::ifstream statm(statm_path);
  if (!statm.is_open()) {
    return 0;
  }
  uint64_t size_pages = 0;
  uint64_t resident_pages = 0;
  statm >> size_pages >> resident_pages;
  long page_size = sysconf(_SC_PAGESIZE);
  return resident_pages * (page_size > 0 ? static_cast<size_t>(page_size) : 4096U);
#endif
}

std::string DeriveWorkerLogPath(const std::string &log_dir, size_t index) {
  const std::string tag = (index == kUnpackerWorkerIndex) ? "unpacker" : std::to_string(index);
  return PathToGenericUtf8(PathFromUtf8(log_dir) / PathFromUtf8("soeasy_worker_" + tag + ".log"));
}

std::string DeriveWorkerDocToTextLogPath(const std::string &configured_path, const std::string &log_dir, size_t index) {
  const std::string suffix =
      (index == kUnpackerWorkerIndex) ? "_unpacker" : ("_worker_" + std::to_string(index));
  if (configured_path.empty()) {
    return PathToGenericUtf8(PathFromUtf8(log_dir) / PathFromUtf8("doctotext" + suffix + ".log"));
  }
  const std::filesystem::path p = PathFromUtf8(configured_path);
  if (!p.has_filename()) {
    return PathToGenericUtf8(p / PathFromUtf8("doctotext" + suffix + ".log"));
  }
  const std::string filename = PathToUtf8(p.stem()) + suffix + PathToUtf8(p.extension());
  return PathToGenericUtf8(p.parent_path() / PathFromUtf8(filename));
}

}  // namespace service
}  // namespace soeasy
