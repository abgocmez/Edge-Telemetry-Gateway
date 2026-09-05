#include "fanout.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>

namespace etg {
namespace {

constexpr std::size_t kDrainBatch = 64;
constexpr int kPollTimeoutMs = 100;

}  // namespace

std::unique_ptr<FanOut> FanOut::create(std::vector<Source*> sources,
                                       std::vector<FrameQueue*> sinks, std::string& error) {
  if (sources.empty()) {
    error = "fan-out needs at least one source";
    return nullptr;
  }
  for (const Source* s : sources) {
    if (s == nullptr) {
      error = "fan-out was given a null source";
      return nullptr;
    }
  }
  if (sinks.empty()) {
    error = "fan-out needs at least one sink";
    return nullptr;
  }
  for (const FrameQueue* q : sinks) {
    if (q == nullptr) {
      error = "fan-out was given a null sink";
      return nullptr;
    }
  }

  const int stop_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (stop_fd < 0) {
    error = std::string{"eventfd: "} + std::strerror(errno);
    return nullptr;
  }

  return std::unique_ptr<FanOut>{new FanOut(std::move(sources), std::move(sinks), stop_fd)};
}

FanOut::FanOut(std::vector<Source*> sources, std::vector<FrameQueue*> sinks, int stop_fd)
    : sources_(std::move(sources)), sinks_(std::move(sinks)), stop_fd_(stop_fd) {}

FanOut::~FanOut() {
  stop();
  if (stop_fd_ >= 0) {
    ::close(stop_fd_);
  }
}

void FanOut::start() {
  if (started_) {
    return;
  }
  started_ = true;
  thread_ = std::thread([this] { run(); });
}

void FanOut::stop() {
  if (!started_) {
    return;
  }
  const std::uint64_t one = 1;
  // Ignoring the result is deliberate: the only failure mode here is a full
  // 64-bit counter, which would mean stop() had been called 2^64 times, and the
  // thread is woken either way.
  const ssize_t written = ::write(stop_fd_, &one, sizeof(one));
  static_cast<void>(written);

  if (thread_.joinable()) {
    thread_.join();
  }
  started_ = false;
}

void FanOut::run() {
  std::array<Frame, kDrainBatch> batch{};
  std::vector<struct pollfd> pfds(sources_.size() + 1);

  for (;;) {
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      pfds[i].fd = sources_[i]->fd();
      pfds[i].events = POLLIN;
      pfds[i].revents = 0;
    }
    pfds.back().fd = stop_fd_;
    pfds.back().events = POLLIN;
    pfds.back().revents = 0;

    const int rc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), kPollTimeoutMs);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      poll_errors_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (rc == 0) {
      continue;
    }

    // Every readable source is drained before the stop is honoured, so frames
    // already sitting in a socket buffer are not thrown away by a shutdown that
    // arrives at the same moment.
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      if ((pfds[i].revents & POLLIN) == 0) {
        continue;
      }
      const std::size_t n = sources_[i]->drain(batch);
      if (n == 0) {
        continue;
      }

      const std::uint64_t base = next_seq_.load(std::memory_order_relaxed);
      for (std::size_t k = 0; k < n; ++k) {
        batch[k].seq = base + k;
      }
      next_seq_.store(base + n, std::memory_order_relaxed);

      const std::span<const Frame> produced{batch.data(), n};
      for (FrameQueue* q : sinks_) {
        // The return value - how many the sink dropped - is deliberately not
        // acted on here. A slow consumer must not be able to change what the
        // ingest thread does; its loss is its own, counted in its own queue.
        static_cast<void>(q->push_batch(produced));
      }

      frames_.fetch_add(n, std::memory_order_relaxed);
      batches_.fetch_add(1, std::memory_order_relaxed);
    }

    if ((pfds.back().revents & POLLIN) != 0) {
      return;
    }
  }
}

FanOut::Stats FanOut::stats() const noexcept {
  Stats s;
  s.frames = frames_.load(std::memory_order_relaxed);
  s.batches = batches_.load(std::memory_order_relaxed);
  s.poll_errors = poll_errors_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace etg
