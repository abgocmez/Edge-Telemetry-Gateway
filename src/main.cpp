#include <cstdio>

#include "version.hpp"

int main() {
  std::printf("edge-telemetry-gateway %s\n", etg::version().data());
  return 0;
}
