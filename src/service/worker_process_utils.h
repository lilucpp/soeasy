#ifndef SOEASY_SRC_SERVICE_WORKER_PROCESS_UTILS_H_
#define SOEASY_SRC_SERVICE_WORKER_PROCESS_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "subprocess.h"

namespace soeasy {
namespace service {

inline constexpr size_t kUnpackerWorkerIndex = std::numeric_limits<size_t>::max();
using WorkerPid = int64_t;

struct SubprocessDeleter {
  void operator()(subprocess_s *proc) const noexcept;
};

using SubprocessHandle = std::unique_ptr<subprocess_s, SubprocessDeleter>;

inline SubprocessHandle MakeSubprocess() {
  return SubprocessHandle(new subprocess_s{}, SubprocessDeleter{});
}

std::vector<const char *> ToArgv(const std::vector<std::string> &args);
std::string ResolveWorkerBinary(const std::string &hint_path);
size_t GetProcessRssBytes(WorkerPid pid);
std::string DeriveWorkerLogPath(const std::string &log_dir, size_t index);
std::string DeriveWorkerDocToTextLogPath(const std::string &configured_path, const std::string &log_dir, size_t index);

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_WORKER_PROCESS_UTILS_H_
