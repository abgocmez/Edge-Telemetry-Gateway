#include "egress.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "source.hpp"
#include "tcp.hpp"
#include "wire.hpp"

namespace etg {
namespace {

constexpr int kPollTimeoutMs = 100;

// Roughly 1600 frames in flight.
//
// Left at the default, the kernel will happily buffer megabytes: a slow consumer
// then reads data that is seconds old while the gateway believes it has "sent"
// it, and the decision about what to discard quietly moves into the socket,
// where nobody counts it and no consumer is told. Bounding it keeps the drop
// where it can be measured and reported - in the gateway's own queue or ring,
// with a marker on the wire - and makes EWOULDBLOCK mean what the back-pressure
// story says it means.
//
// Observed before this was set: 2.9 MB in flight, a consumer 50k frames behind,
// and three gap markers the consumer had not reached by the end of the run.
constexpr int kSendBufferBytes = 64 * 1024;
constexpr std::chrono::milliseconds kPopTimeout{50};

}  // namespace

std::unique_ptr<Egress> Egress::create(std::string name, ConsumerFeed& feed, std::uint16_t port,
                                       Tuning tuning, std::string& error) {
  if (tuning.max_frames == 0 || tuning.max_frames > kMaxBatch) {
    error = "batch size must be between 1 and " + std::to_string(kMaxBatch);
    return nullptr;
  }
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
      new Egress(std::move(name), feed, bound != 0 ? bound : port, tuning, listen_fd, stop_fd)};
}

Egress::Egress(std::string name, ConsumerFeed& feed, std::uint16_t port, Tuning tuning,
               int listen_fd, int stop_fd)
    : name_(std::move(name)),
      feed_(feed),
      port_(port),
      tuning_(tuning),
      listen_fd_(listen_fd),
      stop_fd_(stop_fd) {
  pending_.reserve(wire::kHeaderSize + tuning_.max_frames * wire::kRecordSize);
  reply_buf_.reserve(4096);
}

void Egress::maybe_send_echo() {
  if (tuning_.echo_ms == 0 || client_fd_ < 0) {
    return;
  }
  const std::uint64_t now = monotonic_ns();
  if (now < next_echo_ns_) {
    return;
  }

  // A probe that never came back is written off rather than waited on. Holding
  // the slot would mean one lost reply stops every later measurement, which
  // turns a missing sample into a missing experiment.
  if (echo_outstanding_) {
    echoes_lost_.fetch_add(1, std::memory_order_relaxed);
  }

  ++echo_nonce_;
  echo_sent_ns_ = now;
  echo_outstanding_ = true;
  next_echo_ns_ = now + static_cast<std::uint64_t>(tuning_.echo_ms) * 1'000'000ULL;

  outgoing_.push_back(wire::make_echo(echo_nonce_, echo_sent_ns_));
  echoes_sent_.fetch_add(1, std::memory_order_relaxed);
}

void Egress::drain_replies() {
  if (client_fd_ < 0) {
    return;
  }

  std::array<std::byte, 512> buf{};
  for (;;) {
    const ssize_t n = ::recv(client_fd_, buf.data(), buf.size(), MSG_DONTWAIT);
    if (n <= 0) {
      break;  // EAGAIN, or the peer is gone and flush_pending will find out
    }
    reply_buf_.insert(reply_buf_.end(), buf.begin(), buf.begin() + n);
    if (reply_buf_.size() > 64 * 1024) {
      // A consumer sending anything but echo replies is misbehaving; do not let
      // it grow a buffer here on the gateway's behalf.
      reply_buf_.clear();
    }
  }

  std::size_t off = 0;
  while (reply_buf_.size() - off >= wire::kHeaderSize) {
    wire::BatchHeader header{};
    if (wire::decode_header(wire::ConstHeaderBytes{reply_buf_.data() + off, wire::kHeaderSize},
                            header) != wire::DecodeError::kOk) {
      reply_buf_.clear();  // out of sync; the only safe move is to start over
      return;
    }
    if (reply_buf_.size() - off < wire::kHeaderSize + header.payload_len) {
      break;  // incomplete
    }

    const std::byte* p = reply_buf_.data() + off + wire::kHeaderSize;
    for (std::uint32_t i = 0; i < header.count; ++i) {
      Frame f{};
      if (wire::decode_frame(wire::ConstFrameBytes{p + i * wire::kRecordSize, wire::kRecordSize},
                             f) != wire::DecodeError::kOk) {
        continue;
      }
      if (!wire::is_echo(f) || !echo_outstanding_ || wire::echo_nonce(f) != echo_nonce_) {
        continue;  // stale or unexpected; ignore rather than mismatch a sample
      }
      const std::int64_t rtt =
          static_cast<std::int64_t>(monotonic_ns()) - static_cast<std::int64_t>(echo_sent_ns_);
      {
        const std::lock_guard<std::mutex> lock(rtt_mutex_);
        rtt_.add(rtt);
      }
      echo_outstanding_ = false;
      echoes_returned_.fetch_add(1, std::memory_order_relaxed);
    }
    off += wire::kHeaderSize + header.payload_len;
  }

  if (off > 0) {
    reply_buf_.erase(reply_buf_.begin(), reply_buf_.begin() + static_cast<std::ptrdiff_t>(off));
  }
}

std::size_t Egress::coalesce(std::vector<Frame>& batch, std::size_t have) {
  if (tuning_.linger_us == 0 || have >= tuning_.max_frames) {
    return have;
  }

  const std::uint64_t deadline = monotonic_ns() + tuning_.linger_us * 1000ULL;
  bool waited = false;

  while (have < tuning_.max_frames) {
    const std::uint64_t now = monotonic_ns();
    if (now >= deadline) {
      break;
    }
    const auto remaining =
        std::chrono::milliseconds{static_cast<std::int64_t>((deadline - now) / 1'000'000ULL) + 1};

    const std::size_t more = feed_.pop_batch(
        std::span<Frame>{batch.data() + have, tuning_.max_frames - have}, remaining);
    if (more == 0) {
      break;
    }
    have += more;
    waited = true;
  }

  if (waited) {
    lingered_.fetch_add(1, std::memory_order_relaxed);
  }
  return have;
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
    reply_buf_.clear();
    echo_outstanding_ = false;
    reconnected_ = true;
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
        // Best-effort: a kernel that refuses is no reason to reject the client,
        // it just means more is in flight than intended.
        static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &kSendBufferBytes,
                                       sizeof(kSendBufferBytes)));
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

void Egress::note_gap(std::span<const Frame> frames) {
  if (frames.empty()) {
    return;
  }
  outgoing_.clear();

  if (have_seq_ && frames.front().seq > next_seq_) {
    const std::uint64_t missing = frames.front().seq - next_seq_;
    // The first gap after a reconnect is an absence, not an overrun: this
    // consumer was not there to fall behind. Everything downstream depends on
    // the difference - a cadence monitor suppresses both, but only one of them
    // should ever be counted against the pipeline.
    const GapReason reason =
        reconnected_ ? GapReason::kConsumerAbsent : GapReason::kConsumerOverrun;
    outgoing_.push_back(wire::make_gap(next_seq_, missing, reason, monotonic_ns()));
    gaps_sent_.fetch_add(1, std::memory_order_relaxed);
    if (is_fault(reason)) {
      frames_lost_.fetch_add(missing, std::memory_order_relaxed);
    }
  }
  reconnected_ = false;

  outgoing_.insert(outgoing_.end(), frames.begin(), frames.end());
  next_seq_ = frames.back().seq + 1;
  have_seq_ = true;
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
  std::vector<Frame> batch(tuning_.max_frames);

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

    drain_replies();

    std::size_t n = feed_.pop_batch(
        std::span<Frame>{batch.data(), tuning_.max_frames}, kPopTimeout);
    if (n > 0) {
      n = coalesce(batch, n);
    }
    if (n == 0) {
      if (feed_.closed()) {
        return;
      }
      continue;
    }

    note_gap(std::span<const Frame>{batch.data(), n});
    maybe_send_echo();
    encode_batch(std::span<const Frame>{outgoing_});
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
  s.lingered = lingered_.load(std::memory_order_relaxed);
  s.echoes_sent = echoes_sent_.load(std::memory_order_relaxed);
  s.echoes_returned = echoes_returned_.load(std::memory_order_relaxed);
  s.echoes_lost = echoes_lost_.load(std::memory_order_relaxed);
  {
    const std::lock_guard<std::mutex> lock(rtt_mutex_);
    const Percentiles p = rtt_.compute();
    s.rtt_p50_ns = p.p50;
    s.rtt_p99_ns = p.p99;
    s.rtt_max_ns = p.max;
  }
  s.gaps_sent = gaps_sent_.load(std::memory_order_relaxed);
  s.frames_lost = frames_lost_.load(std::memory_order_relaxed);
  s.connected = connected_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace etg
