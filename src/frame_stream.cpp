#include "frame_stream.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "tcp.hpp"

namespace etg {
namespace {

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::unique_ptr<FrameStream> FrameStream::connect(const std::string& host, std::uint16_t port,
                                                  std::string& error) {
  const int fd = tcp::connect_to(host, port, error);
  if (fd < 0) {
    return nullptr;
  }
  if (!tcp::set_nonblocking(fd, error)) {
    ::close(fd);
    return nullptr;
  }
  return std::unique_ptr<FrameStream>{new FrameStream(fd)};
}

FrameStream::FrameStream(int fd) : fd_(fd) { buf_.resize(wire::kHeaderSize); }

FrameStream::~FrameStream() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

FrameStream::Status FrameStream::fill(std::size_t need, std::int64_t deadline_ms) {
  if (buf_.size() < need) {
    buf_.resize(need);
  }

  while (got_ < need) {
    const ssize_t n = ::read(fd_, buf_.data() + got_, need - got_);
    if (n > 0) {
      got_ += static_cast<std::size_t>(n);
      continue;
    }
    if (n == 0) {
      return Status::kClosed;  // orderly shutdown by the peer
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return Status::kClosed;
    }

    const std::int64_t remaining = deadline_ms - now_ms();
    if (remaining <= 0) {
      // Partial state stays in buf_ and got_, so the next call resumes exactly
      // where this one stopped rather than resynchronising or discarding.
      return Status::kTimeout;
    }

    struct pollfd pfd {};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, static_cast<int>(remaining));
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::kClosed;
    }
    if (rc == 0) {
      return Status::kTimeout;
    }
  }
  return Status::kOk;
}

bool FrameStream::send_back(const Frame& f) {
  std::array<std::byte, wire::kHeaderSize + wire::kRecordSize> msg{};
  wire::encode_header(1, wire::HeaderBytes{msg.data(), wire::kHeaderSize});
  wire::encode_frame(f, wire::FrameBytes{msg.data() + wire::kHeaderSize, wire::kRecordSize});

  std::size_t sent = 0;
  while (sent < msg.size()) {
    const ssize_t n = ::send(fd_, msg.data() + sent, msg.size() - sent,
                             MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    // EWOULDBLOCK included: see the note in the header. A half-written reply is
    // worse than none, so the socket is left alone and the probe is abandoned.
    ++echo_drops_;
    return false;
  }
  return true;
}

FrameStream::Status FrameStream::read_batch(std::vector<Frame>& out, int timeout_ms) {
  out.clear();
  const std::int64_t deadline = now_ms() + timeout_ms;

  if (state_ == State::kHeader) {
    const Status st = fill(wire::kHeaderSize, deadline);
    if (st != Status::kOk) {
      return st;
    }

    wire::BatchHeader header{};
    last_error_ = wire::decode_header(
        wire::ConstHeaderBytes{buf_.data(), wire::kHeaderSize}, header);
    if (last_error_ != wire::DecodeError::kOk) {
      return Status::kProtocolError;
    }

    records_ = header.count;
    need_ = header.payload_len;
    got_ = 0;
    state_ = State::kPayload;

    if (need_ == 0) {
      state_ = State::kHeader;
      need_ = wire::kHeaderSize;
      return Status::kOk;  // a legal, empty batch
    }
  }

  const Status st = fill(need_, deadline);
  if (st != Status::kOk) {
    return st;
  }

  out.reserve(records_);
  for (std::uint32_t i = 0; i < records_; ++i) {
    Frame f{};
    const std::byte* p = buf_.data() + static_cast<std::size_t>(i) * wire::kRecordSize;
    last_error_ = wire::decode_frame(wire::ConstFrameBytes{p, wire::kRecordSize}, f);
    if (last_error_ != wire::DecodeError::kOk) {
      state_ = State::kHeader;
      got_ = 0;
      need_ = wire::kHeaderSize;
      return Status::kProtocolError;
    }
    out.push_back(f);
  }

  state_ = State::kHeader;
  got_ = 0;
  need_ = wire::kHeaderSize;
  return Status::kOk;
}

}  // namespace etg
