#include "ring_ingest.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace etg {
namespace {

constexpr std::size_t kDrainBatch = 64;
constexpr int kPollTimeoutMs = 100;

}  // namespace

std::unique_ptr<RingIngest> RingIngest::create(std::vector<Source*> sources, BroadcastRing& ring,
                                               std::string& error) {
  if (sources.empty()) {
    error = "ring ingest needs at least one source";
    return nullptr;
  }
  for (const Source* s : sources) {
    if (s == nullptr) {
      error = "ring ingest was given a null source";
      return nullptr;
    }
  }

  const int stop_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (stop_fd < 0) {
    error = std::string{"eventfd: "} + std::strerror(errno);
    return nullptr;
  }

  return std::unique_ptr<RingIngest>{new RingIngest(std::move(sources), ring, stop_fd)};
}

RingIngest::RingIngest(std::vector<Source*> sources, BroadcastRing& ring, int stop_fd)
    : sources_(std::move(sources)), ring_(ring), stop_fd_(stop_fd) {}

RingIngest::~RingIngest() {
  stop();
  if (stop_fd_ >= 0) {
    ::close(stop_fd_);
  }
}

void RingIngest::start() {
  if (started_) {
    return;
  }
  started_ = true;
  threads_.reserve(sources_.size());
  for (Source* s : sources_) {
    threads_.emplace_back([this, s] { run(*s); });
  }
}

void RingIngest::stop() {
  if (!started_) {
    return;
  }
  const std::uint64_t one = 1;
  const ssize_t written = ::write(stop_fd_, &one, sizeof(one));
  static_cast<void>(written);

  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  threads_.clear();
  started_ = false;
}

void RingIngest::run(Source& source) {
  std::array<Frame, kDrainBatch> batch{};

  for (;;) {
    std::array<struct pollfd, 2> pfds{};
    pfds[0].fd = source.fd();
    pfds[0].events = POLLIN;
    pfds[1].fd = stop_fd_;
    pfds[1].events = POLLIN;

    const int rc = ::poll(pfds.data(), pfds.size(), kPollTimeoutMs);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      poll_errors_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    // Drain before honouring the stop, so frames already in the socket buffer
    // are not thrown away by a shutdown arriving at the same moment.
    if (rc > 0 && (pfds[0].revents & POLLIN) != 0) {
      const std::size_t n = source.drain(batch);
      if (n > 0) {
        // One RMW for the whole batch. Contiguous tickets also keep this
        // source's frames in the order it produced them, which is exactly the
        // per-source FIFO guarantee the wire format promises - a per-frame loop
        // would give the same result, but only because a single thread claims
        // monotonically, so batching costs nothing and states the intent.
        ring_.publish_batch(std::span<const Frame>{batch.data(), n});
        frames_.fetch_add(n, std::memory_order_relaxed);
        batches_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // The stop eventfd is never read, only polled. Reading it would consume the
    // value and leave the other source threads asleep; leaving it set makes one
    // write wake every thread and keep them woken.
    if (rc > 0 && (pfds[1].revents & POLLIN) != 0) {
      return;
    }
  }
}

RingIngest::Stats RingIngest::stats() const noexcept {
  Stats s;
  s.frames = frames_.load(std::memory_order_relaxed);
  s.batches = batches_.load(std::memory_order_relaxed);
  s.poll_errors = poll_errors_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace etg
