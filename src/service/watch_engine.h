#ifndef SOEASY_SRC_SERVICE_WATCH_ENGINE_H_
#define SOEASY_SRC_SERVICE_WATCH_ENGINE_H_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "efsw/efsw.hpp"
#include "service/event_debouncer.h"
#include "service/index_config.h"

namespace soeasy {
namespace service {

class WatchEngine : public efsw::FileWatchListener {
 public:
  WatchEngine(std::shared_ptr<IndexConfig> config,
              std::shared_ptr<EventDebouncer> debouncer);
  ~WatchEngine() override;

  // Disallow copy and assign
  WatchEngine(const WatchEngine&) = delete;
  WatchEngine& operator=(const WatchEngine&) = delete;

  /**
   * @brief Start watching directories and start event debouncing.
   */
  bool Start();

  /**
   * @brief Stop file watcher and debouncer.
   */
  void Stop();

  /**
   * @brief Check whether file watcher is currently active.
   */
  bool is_watching() const {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    return is_watching_;
  }

  /**
   * @brief Add a directory to watch recursively.
   *
   * @param dir_path Absolute directory path
   * @return true on success, false if path invalid or error
   */
  bool AddWatch(const std::string& dir_path);

  /**
   * @brief Remove an existing directory watch.
   */
  void RemoveWatch(const std::string& dir_path);

  /**
   * @brief Get list of currently monitored root directories.
   */
  std::vector<std::string> GetWatchedDirectories() const;

  /**
   * @brief efsw::FileWatchListener callback implementation.
   */
  void handleFileAction(efsw::WatchID watchid, const std::string& dir,
                        const std::string& filename, efsw::Action action,
                        const std::string& old_filename = "") override;

 private:
  static std::string CombinePath(const std::string& dir,
                                 const std::string& filename);

  std::shared_ptr<IndexConfig> config_;
  std::shared_ptr<EventDebouncer> debouncer_;

  std::unique_ptr<efsw::FileWatcher> file_watcher_;
  mutable std::mutex watch_mutex_;
  std::unordered_map<std::string, efsw::WatchID> watch_ids_;
  bool is_watching_ = false;
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_WATCH_ENGINE_H_
