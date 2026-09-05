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
#include "gap_tracker.hpp"
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

TEST_CASE("a consumer that falls behind is sent a gap marker", "[egress][gap]") {
  // The point of wire version 2, end to end over a real socket.
  //
  // The queue is tiny and far more is pushed than fits, so the consumer is
  // overwritten. What it receives is not merely a jump in sequence numbers: the
  // gateway prepends a marker naming exactly where the loss began and how much
  // of it there was.
  QueueFeed feed{16};
  std::string error;
  auto egress = Egress::create("test", feed, 0, error);
  REQUIRE(egress != nullptr);
  egress->start();

  auto stream = FrameStream::connect("127.0.0.1", egress->port(), error);
  REQUIRE(stream != nullptr);

  // A first batch that fits, so the egress learns where this consumer is.
  std::vector<Frame> first;
  for (std::uint64_t i = 0; i < 4; ++i) {
    first.push_back(make_frame(i));
  }
  static_cast<void>(feed.queue().push_batch(std::span<const Frame>{first}));

  std::vector<Frame> got;
  std::vector<Frame> batch;
  auto pump = [&](std::chrono::milliseconds budget) {
    const auto until = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < until) {
      const FrameStream::Status st = stream->read_batch(batch, 50);
      if (st == FrameStream::Status::kClosed || st == FrameStream::Status::kProtocolError) {
        return;
      }
      got.insert(got.end(), batch.begin(), batch.end());
    }
  };
  pump(1s);
  REQUIRE(got.size() >= 4);
  const std::uint64_t before = got.size();

  // Now overwhelm it: 500 frames into 16 slots.
  std::vector<Frame> flood;
  for (std::uint64_t i = 4; i < 504; ++i) {
    flood.push_back(make_frame(i));
  }
  static_cast<void>(feed.queue().push_batch(std::span<const Frame>{flood}));
  pump(2s);
  egress->stop();

  REQUIRE(got.size() > before);

  std::size_t markers = 0;
  std::uint64_t reported = 0;
  for (std::size_t i = before; i < got.size(); ++i) {
    if (wire::is_gap(got[i])) {
      ++markers;
      reported += wire::gap_count(got[i]);
      // The marker names where the stream resumes, so the next real frame must
      // be exactly there.
      REQUIRE(i + 1 < got.size());
      CHECK(got[i + 1].seq == got[i].seq + wire::gap_count(got[i]));
      CHECK(wire::gap_reason(got[i]) == GapReason::kConsumerOverrun);
      CHECK(got[i].t_kernel_ns == 0);  // a marker was never on a bus
    }
  }

  CHECK(markers >= 1);
  CHECK(reported > 0);
  CHECK(egress->stats().gaps_sent == markers);
  CHECK(egress->stats().frames_lost == reported);
}

TEST_CASE("a consumer that keeps up is sent no markers at all", "[egress][gap]") {
  QueueFeed feed{4096};
  std::string error;
  auto egress = Egress::create("test", feed, 0, error);
  REQUIRE(egress != nullptr);
  egress->start();

  auto stream = FrameStream::connect("127.0.0.1", egress->port(), error);
  REQUIRE(stream != nullptr);

  std::vector<Frame> sent;
  for (std::uint64_t i = 0; i < 300; ++i) {
    sent.push_back(make_frame(i));
  }
  static_cast<void>(feed.queue().push_batch(std::span<const Frame>{sent}));

  const std::vector<Frame> got = collect(*stream, sent.size(), 5s);
  egress->stop();

  REQUIRE(got.size() == sent.size());
  for (const Frame& f : got) {
    CHECK_FALSE(wire::is_gap(f));
  }
  CHECK(egress->stats().gaps_sent == 0);
  CHECK(egress->stats().frames_lost == 0);
}

TEST_CASE("a consumer that disappears and comes back is told it was away, not that it failed",
          "[egress][gap][recovery]") {
  // The restart case, which is not the same as the overrun case and must not be
  // reported as one.
  //
  // A consumer that was killed did not lose anything it was entitled to - it did
  // not exist. But it still has to be told there is a hole, because a recorder
  // appending to a file must not leave an unexplained jump in it: a capture that
  // silently skips is a recording that lies about what it contains, and nothing
  // downstream can tell that apart from corruption.
  //
  // The queue is small on purpose. A consumer that comes back quickly enough
  // loses nothing at all - the buffer simply held its frames - and no marker is
  // sent because there is no hole to describe. The marker appears only once the
  // outage outlasts the buffer, which is the honest condition for it.
  QueueFeed feed{64};
  std::string error;
  auto egress = Egress::create("test", feed, 0, error);
  REQUIRE(egress != nullptr);
  egress->start();

  const auto push = [&](std::uint64_t from, std::uint64_t to) {
    std::vector<Frame> v;
    for (std::uint64_t i = from; i < to; ++i) {
      v.push_back(make_frame(i));
    }
    static_cast<void>(feed.queue().push_batch(std::span<const Frame>{v}));
  };

  {
    auto stream = FrameStream::connect("127.0.0.1", egress->port(), error);
    REQUIRE(stream != nullptr);
    push(0, 50);
    const std::vector<Frame> got = collect(*stream, 50, 5s);
    REQUIRE(got.size() == 50);
    for (const Frame& f : got) {
      CHECK_FALSE(wire::is_gap(f));
    }
  }  // the consumer is killed here

  // Wait for the gateway to notice, so the reconnect below is genuinely a new
  // connection rather than the same one.
  for (int i = 0; i < 100 && egress->stats().disconnects == 0; ++i) {
    std::this_thread::sleep_for(20ms);
  }
  REQUIRE(egress->stats().disconnects >= 1);

  // 400 frames go past while nobody is listening, into a 64-slot queue: most
  // of them are overwritten and genuinely gone.
  push(50, 450);

  auto stream = FrameStream::connect("127.0.0.1", egress->port(), error);
  REQUIRE(stream != nullptr);
  push(450, 500);

  std::vector<Frame> got;
  std::vector<Frame> batch;
  const auto until = std::chrono::steady_clock::now() + 5s;
  while (got.empty() && std::chrono::steady_clock::now() < until) {
    const FrameStream::Status st = stream->read_batch(batch, 100);
    if (st == FrameStream::Status::kClosed || st == FrameStream::Status::kProtocolError) {
      break;
    }
    got.insert(got.end(), batch.begin(), batch.end());
  }
  egress->stop();

  REQUIRE_FALSE(got.empty());
  REQUIRE(wire::is_gap(got.front()));

  // Absent, not overrun. A cadence monitor suppresses both, but only one of them
  // should ever be counted against the pipeline.
  CHECK(wire::gap_reason(got.front()) == GapReason::kConsumerAbsent);
  CHECK_FALSE(is_fault(GapReason::kConsumerAbsent));
  CHECK(got.front().seq == 50);  // where this consumer's stream stopped

  // The marker still names the hole exactly, so the file stays honest.
  CHECK(got.front().seq + wire::gap_count(got.front()) == got[1].seq);

  // Counted as a marker, but not charged to the pipeline as lost frames.
  CHECK(egress->stats().gaps_sent == 1);
  CHECK(egress->stats().frames_lost == 0);
}

TEST_CASE("an absence is a discontinuity but not loss, to a consumer", "[gap][recovery]") {
  GapTracker t;
  t.observe(make_frame(0));
  t.observe(make_frame(1));

  t.observe(wire::make_gap(2, 400, GapReason::kConsumerAbsent, 0));
  t.observe(make_frame(402));

  CHECK(t.stats().markers == 1);
  CHECK(t.stats().absent_markers == 1);
  CHECK(t.stats().absent_frames == 400);

  // The 400 frames are accounted for and reported, and they are not loss: a
  // consumer that was restarted has nothing to alarm about, and a drop rate
  // that counted them would be inflated by every restart.
  CHECK(t.stats().reported_lost == 0);
  CHECK(t.total_lost() == 0);

  // And the stream is picked up exactly where the marker said it would resume,
  // so no silent jump is invented.
  CHECK(t.stats().silent_jumps == 0);
  CHECK(t.stats().frames == 3);
}
