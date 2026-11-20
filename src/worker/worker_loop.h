#ifndef SOEASY_SRC_WORKER_WORKER_LOOP_H_
#define SOEASY_SRC_WORKER_WORKER_LOOP_H_

#include <string>

namespace soeasy {
namespace worker {

class ParserRegistry;

#ifdef _WIN32
/**
 * @brief Run the worker message processing loop using Cap'n Proto RPC over a Win32 named pipe.
 *
 * @param pipe_handle Connected Win32 pipe HANDLE to master
 * @param custom_registry Optional custom parser registry (nullptr to use default)
 * @return int Exit status code
 */
int RunWorkerLoop(void *pipe_handle, ParserRegistry *custom_registry = nullptr);
#else
/**
 * @brief Run the worker message processing loop using Cap'n Proto RPC over Unix domain socket.
 *
 * @param socket_fd Connected Unix domain stream socket to master
 * @param custom_registry Optional custom parser registry (nullptr to use default)
 * @return int Exit status code
 */
int RunWorkerLoop(int socket_fd, ParserRegistry *custom_registry = nullptr);
#endif

}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_WORKER_LOOP_H_
