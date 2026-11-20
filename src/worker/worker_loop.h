#ifndef SOEASY_SRC_WORKER_WORKER_LOOP_H_
#define SOEASY_SRC_WORKER_WORKER_LOOP_H_

#include <string>

namespace soeasy {
namespace worker {

class ParserRegistry;

/**
 * @brief Run the worker message processing loop using Cap'n Proto RPC over kj::Network.
 *
 * @param rpc_addr Target master RPC address (e.g. "127.0.0.1:54321")
 * @param custom_registry Optional custom parser registry (nullptr to use default)
 * @return int Exit status code
 */
int RunWorkerLoop(const std::string &rpc_addr, ParserRegistry *custom_registry = nullptr);

}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_WORKER_LOOP_H_
