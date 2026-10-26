#ifndef SOEASY_SRC_WORKER_WORKER_LOOP_H_
#define SOEASY_SRC_WORKER_WORKER_LOOP_H_

#include <string>

namespace soeasy {
namespace worker {

/**
 * @brief Run the worker message processing loop until socket disconnect.
 *
 * @param socket_fd Connected Unix domain stream socket to master
 * @param shm_name Shared memory pool name
 * @param slot_count Number of slots
 * @param slot_capacity Capacity per slot in bytes
 * @return int Exit status code
 */
int RunWorkerLoop(int socket_fd, const std::string& shm_name,
                  size_t slot_count, size_t slot_capacity);

}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_WORKER_LOOP_H_
