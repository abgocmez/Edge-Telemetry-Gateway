#include "rt.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstring>

namespace etg::rt {

bool lock_memory(std::string& error) {
  if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    error = std::string{"mlockall: "} + std::strerror(errno);
    if (errno == ENOMEM || errno == EPERM) {
      error += " (needs CAP_IPC_LOCK or a raised RLIMIT_MEMLOCK)";
    }
    return false;
  }
  return true;
}

}  // namespace etg::rt
