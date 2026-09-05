#include <unistd.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "egress.hpp"
#include "feed.hpp"
#include "frame_stream.hpp"
#include "tcp.hpp"
#include "wire.hpp"

using namespace etg;
using namespace std::chrono_literals;

namespace {

Frame make_frame(std::uint64_t seq) {
  Frame f{};
  f.seq = seq;
  f.t_kernel_ns = 1'700'000'000'000'000'000ULL + seq;
  f.t_ingest_ns = 42'000 + seq;
  f.can_id = 0x100 + static_cast<std::uint32_t>(seq % 8);
  f.src_id = static_cast<std::uint8_t>(seq % 4);
  f.len = 8;
  f.flags = 0;
  f.data = {1, 2, 3, 4, 5, 6, 7, 8};
  return f;
}

// Reads until `want` frames have arrived or the deadline passes.
std::vector<Frame> collect(FrameStream& s, std::size_t want, std::chrono::milliseconds budget) {
  std::vector<Frame> all;
  std::vector<Frame> batch;
  const auto until = std::chrono::steady_clock::now() + budget;
  while (all.size() < want && std::chrono::steady_clock::now() < until) {
    const FrameStream::Status st = s.read_batch(batch, 100);
    if (st == FrameStream::Status::kClosed || st == FrameStream::Status::kProtocolError) {
      break;
    }
    all.insert(all.end(), batch.begin(), batch.end());
  }
  return all;
}

}  // namespace

TEST_CASE("frames survive the round trip over a real socket", "[egress][wire]") {
  QueueFeed feed{4096};
  std::string error;
  auto egress = Egress::create("test", feed, 0, error);  // port 0: kernel picks
  REQUIRE(egress != nullptr);
  REQUIRE(egress->port() != 0);
  egress->start();

  auto stream = FrameStream::connect("127.0.0.1", egress->port(), error);
  REQUIRE(stream != nullptr);

  std::vector<Frame> sent;
  sent.reserve(500);
  for (std::uint64_t i = 0; i < 500; ++i) {
    sent.push_back(make_frame(i));
  }
  static_cast<void>(feed.queue().push_batch(std::span<const Frame>{sent}));

  const std::vector<Frame> got = collect(*stream, sent.size(), 5s);
  REQUIRE(got.size() == sent.size());

  for (std::size_t i = 0; i < sent.size(); ++i) {
    CHECK(got[i].seq == sent[i].seq);
    CHECK(got[i].can_id == sent[i].can_id);
    CHECK(got[i].src_id == sent[i].src_id);
    CHECK(got[i].len == sent[i].len);
    CHECK(got[i].t_kernel_ns == sent[i].t_kernel_ns);
    CHECK(got[i].t_ingest_ns == sent[i].t_ingest_ns);
    CHECK(got[i].data == sent[i].data);
  }

  egress->stop();
}

TEST_CASE("a batch split across many reads is reassembled", "[wire][stream]") {
  // TCP is a byte stream: a read can land halfway through a header or halfway
  // through a record. Feeding the bytes a few at a time is the only way to
  // prove the parser holds partial state instead of assuming one read is one
  // message - the assumption that survives testing and fails under load.
  std::string error;
  const int listen_fd = tcp::listen_on(0, error);
  REQUIRE(listen_fd >= 0);
  const std::uint16_t port = tcp::local_port(listen_fd);
  REQUIRE(port != 0);

  constexpr std::uint32_t kCount = 40;
  std::vector<std::byte> bytes(wire::kHeaderSize + kCount * wire::kRecordSize);
  wire::encode_header(kCount, wire::HeaderBytes{bytes.data(), wire::kHeaderSize});
  for (std::uint32_t i = 0; i < kCount; ++i) {
    wire::encode_frame(make_frame(i),
                       wire::FrameBytes{bytes.data() + wire::kHeaderSize + i * wire::kRecordSize,
                                        wire::kRecordSize});
  }

  std::thread server([&] {
    for (;;) {
      bool would_block = false;
      std::string err;
      const int fd = tcp::accept_nonblocking(listen_fd, would_block, err);
      if (fd >= 0) {
        // Seven bytes at a time: every header field and every record boundary
        // ends up straddling a read.
        for (std::size_t off = 0; off < bytes.size(); off += 7) {
          const std::size_t n = std::min<std::size_t>(7, bytes.size() - off);
          // Not asserted on: Catch2 macros are not safe from a non-main
          // thread. A short write would show up as a missing frame below.
          const ssize_t w = ::write(fd, bytes.data() + off, n);
          static_cast<void>(w);
          std::this_thread::sleep_for(200us);
        }
        std::this_thread::sleep_for(200ms);
        ::close(fd);
        return;
      }
      std::this_thread::sleep_for(5ms);
    }
  });

  auto stream = FrameStream::connect("127.0.0.1", port, error);
  REQUIRE(stream != nullptr);

  const std::vector<Frame> got = collect(*stream, kCount, 10s);
  server.join();
  ::close(listen_fd);

  REQUIRE(got.size() == kCount);
  for (std::uint32_t i = 0; i < kCount; ++i) {
    CHECK(got[i].seq == i);
    CHECK(got[i].can_id == 0x100 + (i % 8));
  }
}

TEST_CASE("a stream of garbage is a protocol error, not a crash", "[wire][stream]") {
  std::string error;
  const int listen_fd = tcp::listen_on(0, error);
  REQUIRE(listen_fd >= 0);
  const std::uint16_t port = tcp::local_port(listen_fd);

  std::thread server([&] {
    for (;;) {
      bool would_block = false;
      std::string err;
      const int fd = tcp::accept_nonblocking(listen_fd, would_block, err);
      if (fd >= 0) {
        const std::array<std::uint8_t, 16> junk{0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x10, 0x28, 0x00,
                                                0x01, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00};
        const ssize_t w = ::write(fd, junk.data(), junk.size());
        static_cast<void>(w);
        std::this_thread::sleep_for(200ms);
        ::close(fd);
        return;
      }
      std::this_thread::sleep_for(5ms);
    }
  });

  auto stream = FrameStream::connect("127.0.0.1", port, error);
  REQUIRE(stream != nullptr);

  std::vector<Frame> batch;
  FrameStream::Status st = FrameStream::Status::kTimeout;
  for (int i = 0; i < 20 && st == FrameStream::Status::kTimeout; ++i) {
    st = stream->read_batch(batch, 100);
  }
  server.join();
  ::close(listen_fd);

  REQUIRE(st == FrameStream::Status::kProtocolError);
  CHECK(stream->last_error() == wire::DecodeError::kBadMagic);
}

TEST_CASE("a consumer that leaves is noticed, and the queue keeps its place",
          "[egress][failure]") {
  QueueFeed feed{64};
  std::string error;
  auto egress = Egress::create("test", feed, 0, error);
  REQUIRE(egress != nullptr);
  egress->start();

  {
    auto stream = FrameStream::connect("127.0.0.1", egress->port(), error);
    REQUIRE(stream != nullptr);

    std::vector<Frame> first;
    for (std::uint64_t i = 0; i < 10; ++i) {
      first.push_back(make_frame(i));
    }
    static_cast<void>(feed.queue().push_batch(std::span<const Frame>{first}));
    const std::vector<Frame> got = collect(*stream, first.size(), 5s);
    REQUIRE(got.size() == first.size());
  }  // consumer disappears here

  // Frames arriving with nobody connected are queued, then dropped once the
  // queue is full. They are deliberately not drained and discarded: a consumer
  // that returns should get the freshest data, and be told what it missed.
  std::vector<Frame> during;
  for (std::uint64_t i = 10; i < 500; ++i) {
    during.push_back(make_frame(i));
  }
  static_cast<void>(feed.queue().push_batch(std::span<const Frame>{during}));

  bool saw_disconnect = false;
  for (int i = 0; i < 100 && !saw_disconnect; ++i) {
    saw_disconnect = egress->stats().disconnects > 0;
    std::this_thread::sleep_for(20ms);
  }
  CHECK(saw_disconnect);
  CHECK(feed.stats().dropped > 0);

  egress->stop();
}

TEST_CASE("egress starts disconnected and stop is idempotent", "[egress]") {
  QueueFeed feed{64};
  std::string error;
  auto egress = Egress::create("test", feed, 0, error);
  REQUIRE(egress != nullptr);

  CHECK_FALSE(egress->stats().connected);
  CHECK(egress->stats().connects == 0);

  egress->start();
  std::this_thread::sleep_for(50ms);
  egress->stop();
  egress->stop();

  CHECK(egress->stats().frames_sent == 0);
}
