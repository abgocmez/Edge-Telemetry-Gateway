#include "synthetic_source.hpp"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace etg {

std::unique_ptr<SyntheticSource> SyntheticSource::open(const Config& cfg, std::string& error) {
  if (cfg.can_ids.empty()) {
    error = "synthetic source needs at least one CAN id";
    return nullptr;
  }
  if (cfg.period_ns == 0 || cfg.frames_per_tick == 0) {
    error = "synthetic source needs a non-zero period and frames_per_tick";
    return nullptr;
  }

  const int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0) {
    error = std::string{"timerfd_create: "} + std::strerror(errno);
    return nullptr;
  }

  struct itimerspec spec {};
  spec.it_interval.tv_sec = cfg.period_ns / 1'000'000'000U;
  spec.it_interval.tv_nsec = cfg.period_ns % 1'000'000'000U;
  spec.it_value = spec.it_interval;
  if (::timerfd_settime(fd, 0, &spec, nullptr) < 0) {
    error = std::string{"timerfd_settime: "} + std::strerror(errno);
    ::close(fd);
    return nullptr;
  }

  return std::unique_ptr<SyntheticSource>{new SyntheticSource(cfg, fd)};
}

SyntheticSource::SyntheticSource(const Config& cfg, int timer_fd)
    : cfg_(cfg), timer_fd_(timer_fd), name_("synth" + std::to_string(cfg.src_id)) {}

SyntheticSource::~SyntheticSource() {
  if (timer_fd_ >= 0) {
    ::close(timer_fd_);
  }
}

std::size_t SyntheticSource::drain(std::span<Frame> out) noexcept {
  // timerfd reports how many expirations were missed, so a tick lost to
  // scheduling is owed rather than silently skipped. That is what keeps the
  // achieved rate honest under load.
  std::uint64_t expirations = 0;
  const ssize_t n = ::read(timer_fd_, &expirations, sizeof(expirations));
  if (n == static_cast<ssize_t>(sizeof(expirations))) {
    pending_ += expirations * cfg_.frames_per_tick;
  } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    ++stats_.read_errors;
  }

  const std::size_t want = out.size() < pending_ ? out.size() : static_cast<std::size_t>(pending_);

  const std::uint64_t t_kernel = realtime_ns();
  const std::uint64_t t_ingest = monotonic_ns();

  for (std::size_t i = 0; i < want; ++i) {
    Frame& f = out[i];
    f.seq = 0;  // assigned at ring-claim time
    f.t_kernel_ns = t_kernel;
    f.t_ingest_ns = t_ingest;
    f.can_id = cfg_.can_ids[counter_ % cfg_.can_ids.size()];
    f.src_id = cfg_.src_id;
    f.len = 8;
    f.flags = 0;
    f.reserved = 0;
    for (std::size_t b = 0; b < kMaxPayload; ++b) {
      f.data[b] = static_cast<std::uint8_t>((counter_ >> (8U * b)) & 0xFFU);
    }
    ++counter_;
  }

  pending_ -= want;
  stats_.frames += want;
  return want;
}

}  // namespace etg
