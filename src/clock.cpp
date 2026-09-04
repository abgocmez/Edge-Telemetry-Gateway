#include <ctime>

#include "source.hpp"

namespace etg {
namespace {

std::uint64_t now_ns(clockid_t clk) noexcept {
  struct timespec ts {};
  ::clock_gettime(clk, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

}  // namespace

std::uint64_t monotonic_ns() noexcept { return now_ns(CLOCK_MONOTONIC); }
std::uint64_t realtime_ns() noexcept { return now_ns(CLOCK_REALTIME); }

}  // namespace etg
