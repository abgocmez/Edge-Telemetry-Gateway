#include "egress.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "tcp.hpp"
#include "wire.hpp"

namespace etg {
namespace {

constexpr int kPollTimeoutMs = 100;
constexpr std::chrono::milliseconds kPopTimeout{50};

}  // namespace

std::unique_ptr<Egress> Egress::create(std::string name, ConsumerFeed& feed, std::uint16_t port,
                                       std::string& error) {
  const int listen_fd = tcp::listen_on(port, error);
  if (listen_fd < 0) {
    return nullptr;
  }

  const int stop_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (stop_fd < 0) {
    error = std::string{"eventfd: "} + std::strerror(errno);
    ::close(listen_fd);
    return nullptr;
  }

  // Port 0 means the kernel picked one; report what it actually chose.
  const std::uint16_t bound = tcp::local_port(listen_fd);
  return std::unique_ptr<Egress>{
      new Egress(std::move(name), feed, bound != 0 ? bound : port, listen_fd, stop_fd)};
}

Egress::Egress(std::string name, ConsumerFeed& feed, std::uint16_t port, int listen_fd, int stop_fd)
    : name_(std::move(name)), feed_(feed), port_(port), listen_fd_(listen_fd), stop_fd_(stop_fd) {
  pending_.reserve(wire::kHeaderSize + kMaxBatch * wire::kRecordSize);
}

Egress::~Egress() {
  stop();
  drop_client();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
  }
  if (stop_fd_ >= 0) {
    ::close(stop_fd_);
  }
}

void Egress::start() {
  if (started_) {
    return;
  }
  started_ = true;
  thread_ = std::thread([this] { run(); });
}

void Egress::stop() {
  if (!started_) {
    return;
  }
  const std::uint64_t one = 1;
  // Assigned rather than cast to void: write() is warn_unused_result, and a
  // cast does not suppress it. The only failure here would be a full 64-bit
  // counter, and the thread is woken by the queue close either way.
  const ssize_t written = ::write(stop_fd_, &one, sizeof(one));
  static_cast<void>(written);
  // Also wake a thread parked in pop_batch; the eventfd alone only covers the
  // poll paths, and the feed has its own idle mechanism whichever topology it is.
  feed_.close();
  if (thread_.joinable()) {
    thread_.join();
  }
  started_ = false;
}

void Egress::drop_client() {
  if (client_fd_ >= 0) {
    ::close(client_fd_);
    client_fd_ = -1;
    pending_.clear();
    pending_offset_ = 0;
    connected_.store(false, std::memory_order_relaxed);
    disconnects_.fetch_add(1, std::memory_order_relaxed);
  }
}

bool Egress::wait_for_client() {
  while (client_fd_ < 0) {
    std::array<struct pollfd, 2> pfds{};
    pfds[0].fd = listen_fd_;
    pfds[0].events = POLLIN;
    pfds[1].fd = stop_fd_;
    pfds[1].events = POLLIN;

    const int rc = ::poll(pfds.data(), pfds.size(), kPollTimeoutMs);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (rc > 0 && (pfds[1].revents & POLLIN) != 0) {
      return false;
    }
    if (rc > 0 && (pfds[0].revents & POLLIN) != 0) {
      bool would_block = false;
      std::string error;
      const int fd = tcp::accept_nonblocking(listen_fd_, would_block, error);
      if (fd >= 0) {
        client_fd_ = fd;
        connected_.store(true, std::memory_order_relaxed);
        connects_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  return true;
}

// A departed consumer is otherwise invisible until a write fails, and the first
// write after the peer closes usually succeeds - EPIPE only arrives on the
// second. On a quiet bus that could be never, leaving `connected` reporting a
// consumer that left minutes ago. POLLRDHUP sees the FIN without writing.
bool Egress::client_gone() {
  if (client_fd_ < 0) {
    return true;
  }
  struct pollfd pfd {};
  pfd.fd = client_fd_;
  pfd.events = POLLRDHUP;
  const int rc = ::poll(&pfd, 1, 0);
  if (rc <= 0) {
    return false;
  }
  return (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
}

void Egress::encode_batch(std::span<const Frame> frames) {
  pending_.resize(wire::kHeaderSize + frames.size() * wire::kRecordSize);
  pending_offset_ = 0;

  wire::encode_header(static_cast<std::uint32_t>(frames.size()),
                      wire::HeaderBytes{pending_.data(), wire::kHeaderSize});

  std::byte* p = pending_.data() + wire::kHeaderSize;
  for (const Frame& f : frames) {
    wire::encode_frame(f, wire::FrameBytes{p, wire::kRecordSize});
    p += wire::kRecordSize;
  }
}

bool Egress::flush_pending() {
  while (pending_offset_ < pending_.size()) {
    const std::size_t remaining = pending_.size() - pending_offset_;
    // MSG_NOSIGNAL: a consumer that vanishes must return EPIPE here, not kill
    // the whole gateway with SIGPIPE.
    const ssize_t n = ::send(client_fd_, pending_.data() + pending_offset_, remaining,
                             MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      pending_offset_ += static_cast<std::size_t>(n);
      bytes_sent_.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
      if (static_cast<std::size_t>(n) < remaining) {
        // A partial write is normal, not an error. Handling it by retrying the
        // remainder from an offset is the whole reason this buffer exists.
        partial_writes_.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // The consumer is not draining fast enough. Park on POLLOUT rather than
      // spinning; the queue absorbs the arriving frames and drops the oldest.
      would_block_.fetch_add(1, std::memory_order_relaxed);

      std::array<struct pollfd, 2> pfds{};
      pfds[0].fd = client_fd_;
      pfds[0].events = POLLOUT;
      pfds[1].fd = stop_fd_;
      pfds[1].events = POLLIN;

      const int rc = ::poll(pfds.data(), pfds.size(), kPollTimeoutMs);
      if (rc < 0 && errno != EINTR) {
        return false;
      }
      if (rc > 0 && (pfds[1].revents & POLLIN) != 0) {
        return false;
      }
      continue;
    }

    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;  // EPIPE, ECONNRESET, or the peer closed
  }

  pending_.clear();
  pending_offset_ = 0;
  return true;
}

void Egress::run() {
  std::vector<Frame> batch(kMaxBatch);

  for (;;) {
    if (!wait_for_client()) {
      return;
    }

    if (!pending_.empty()) {
      if (!flush_pending()) {
        drop_client();
        continue;
      }
    }

    if (client_gone()) {
      drop_client();
      continue;
    }

    const std::size_t n = feed_.pop_batch(std::span<Frame>{batch}, kPopTimeout);
    if (n == 0) {
      if (feed_.closed()) {
        return;
      }
      continue;
    }

    encode_batch(std::span<const Frame>{batch.data(), n});
    if (!flush_pending()) {
      drop_client();
      continue;
    }

    frames_sent_.fetch_add(n, std::memory_order_relaxed);
    batches_sent_.fetch_add(1, std::memory_order_relaxed);
  }
}

Egress::Stats Egress::stats() const noexcept {
  Stats s;
  s.frames_sent = frames_sent_.load(std::memory_order_relaxed);
  s.batches_sent = batches_sent_.load(std::memory_order_relaxed);
  s.bytes_sent = bytes_sent_.load(std::memory_order_relaxed);
  s.would_block = would_block_.load(std::memory_order_relaxed);
  s.partial_writes = partial_writes_.load(std::memory_order_relaxed);
  s.connects = connects_.load(std::memory_order_relaxed);
  s.disconnects = disconnects_.load(std::memory_order_relaxed);
  s.connected = connected_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace etg
