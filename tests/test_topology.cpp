#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "broadcast_ring.hpp"
#include "egress.hpp"
#include "fanout.hpp"
#include "feed.hpp"
#include "frame_stream.hpp"
#include "ring_ingest.hpp"
#include "synthetic_source.hpp"

using namespace etg;
using namespace std::chrono_literals;

namespace {

std::unique_ptr<SyntheticSource> fast_source(std::uint8_t id) {
  SyntheticSource::Config cfg;
  cfg.src_id = id;
  cfg.period_ns = 200'000;  // 5 kHz
  cfg.frames_per_tick = 1;
  cfg.can_ids = {static_cast<std::uint32_t>(0x100 + id)};
  std::string error;
  auto src = SyntheticSource::open(cfg, error);
  REQUIRE(src != nullptr);
  return src;
}

Frame make_frame(std::uint64_t counter) {
  Frame f{};
  f.t_ingest_ns = counter;
  f.can_id = static_cast<std::uint32_t>(counter & 0x7FFU);
  f.len = 8;
  return f;
}

std::vector<Frame> drain(ConsumerFeed& feed, std::chrono::milliseconds budget) {
  std::vector<Frame> all;
  std::vector<Frame> buf(256);
  const auto until = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < until) {
    const std::size_t n = feed.pop_batch(buf, 10ms);
    if (n == 0) {
      break;
    }
    all.insert(all.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
  }
  return all;
}

}  // namespace

TEST_CASE("both feeds report which topology they are", "[feed]") {
  BroadcastRing ring{64};
  QueueFeed q{64};
  RingFeed r{ring};
  CHECK(std::string{q.topology()} == "queue");
  CHECK(std::string{r.topology()} == "ring");
  CHECK(q.stats().capacity == 64);
  CHECK(r.stats().capacity == 64);
}

TEST_CASE("both feeds deliver the same frames in the same order", "[feed]") {
  // The interchangeability claim, checked rather than asserted in a comment.
  // Egress is written against ConsumerFeed precisely so the two topologies can
  // be swapped under identical work; if they disagreed here, every comparison
  // between them later would be measuring two different systems.
  constexpr std::uint64_t kCount = 500;

  BroadcastRing ring{4096};
  QueueFeed qf{4096};
  RingFeed rf{ring};

  std::vector<Frame> batch;
  for (std::uint64_t i = 0; i < kCount; ++i) {
    batch.push_back(make_frame(i));
  }
  static_cast<void>(qf.queue().push_batch(std::span<const Frame>{batch}));
  ring.publish_batch(std::span<const Frame>{batch});

  const std::vector<Frame> from_queue = drain(qf, 2s);
  const std::vector<Frame> from_ring = drain(rf, 2s);

  REQUIRE(from_queue.size() == kCount);
  REQUIRE(from_ring.size() == kCount);
  for (std::uint64_t i = 0; i < kCount; ++i) {
    CHECK(from_queue[i].t_ingest_ns == i);
    CHECK(from_ring[i].t_ingest_ns == i);
    // The ring assigns the sequence from its ticket; the queue carries whatever
    // the fan-out thread wrote. Both are dense and ordered, which is the only
    // thing a consumer is promised.
    CHECK(from_ring[i].seq == i);
  }

  CHECK(qf.stats().delivered == kCount);
  CHECK(rf.stats().delivered == kCount);
  CHECK(qf.stats().dropped == 0);
  CHECK(rf.stats().dropped == 0);
}

TEST_CASE("a ring feed reports its own loss", "[feed]") {
  BroadcastRing ring{16};
  RingFeed rf{ring};

  for (std::uint64_t i = 0; i < 100; ++i) {
    ring.publish(make_frame(i));
  }

  const std::vector<Frame> got = drain(rf, 1s);
  CHECK(got.size() == 16);
  // 100 published into 16 cells. The reader was not told about the loss by
  // anyone; it worked it out from the sequence numbers.
  CHECK(rf.stats().dropped == 84);
  CHECK(got.front().seq == 84);
}

TEST_CASE("a ring feed attaches to live data, not to the beginning", "[feed]") {
  BroadcastRing ring{16};
  for (std::uint64_t i = 0; i < 100; ++i) {
    ring.publish(make_frame(i));
  }

  RingFeed rf{ring};  // attaches here, after 84 frames are already gone
  const std::vector<Frame> got = drain(rf, 1s);

  CHECK(got.size() == 16);
  CHECK(got.front().seq == 84);
  // Not reported as loss: a consumer that connects to a running gateway did not
  // miss those frames, it simply did not exist yet.
  CHECK(rf.stats().dropped == 0);
}

TEST_CASE("closing a feed wakes a reader parked in pop_batch", "[feed]") {
  auto check_wakes = [](ConsumerFeed& feed) {
    std::atomic<bool> returned{false};
    std::thread reader([&] {
      std::vector<Frame> buf(8);
      static_cast<void>(feed.pop_batch(buf, 10s));
      returned.store(true);
    });

    std::this_thread::sleep_for(80ms);
    CHECK_FALSE(returned.load());

    const auto begin = std::chrono::steady_clock::now();
    feed.close();
    reader.join();

    CHECK(returned.load());
    // Woken by close, not by the ten-second timeout expiring. Shutdown that
    // waits out a timeout is shutdown that looks like a hang.
    CHECK(std::chrono::steady_clock::now() - begin < 5s);
  };

  SECTION("queue") {
    QueueFeed q{64};
    check_wakes(q);
  }
  SECTION("ring") {
    BroadcastRing ring{64};
    RingFeed r{ring};
    check_wakes(r);
  }
}

TEST_CASE("an idle ring feed does not spin", "[feed]") {
  BroadcastRing ring{64};
  RingFeed rf{ring};
  std::vector<Frame> buf(8);

  const auto begin = std::chrono::steady_clock::now();
  const std::size_t n = rf.pop_batch(buf, 200ms);
  const auto elapsed = std::chrono::steady_clock::now() - begin;

  CHECK(n == 0);
  // It waited rather than returning instantly, which is what proves it parked
  // instead of polling in a loop and burning a core for the whole timeout.
  CHECK(elapsed >= 150ms);
}

TEST_CASE("a producer wakes a parked ring reader promptly", "[feed]") {
  BroadcastRing ring{64};
  RingFeed rf{ring};

  std::thread producer([&] {
    std::this_thread::sleep_for(60ms);
    ring.publish(make_frame(7));
  });

  std::vector<Frame> buf(8);
  const auto begin = std::chrono::steady_clock::now();
  const std::size_t n = rf.pop_batch(buf, 5s);
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  producer.join();

  REQUIRE(n == 1);
  CHECK(buf[0].t_ingest_ns == 7);
  // The wake has to come from the notify, not from the timeout: the whole point
  // of the idle path is that a quiet feed costs neither a core nor latency.
  CHECK(elapsed < 1s);
}

TEST_CASE("ring ingest refuses a configuration it cannot honour", "[ingest]") {
  BroadcastRing ring{64};
  std::string error;
  CHECK(RingIngest::create({}, ring, error) == nullptr);
  CHECK_FALSE(error.empty());
  CHECK(RingIngest::create({nullptr}, ring, error) == nullptr);
}

TEST_CASE("several sources publish into one ring and every reader sees all of them",
          "[ingest][stress]") {
  // The configuration that makes the ring's multi-producer machinery real: three
  // buses, three ingest threads, one ring, two independent consumers.
  auto s0 = fast_source(0);
  auto s1 = fast_source(1);
  auto s2 = fast_source(2);

  BroadcastRing ring{1 << 14U};
  RingFeed a{ring};
  RingFeed b{ring};

  std::string error;
  auto ingest = RingIngest::create({s0.get(), s1.get(), s2.get()}, ring, error);
  REQUIRE(ingest != nullptr);
  CHECK(ingest->source_count() == 3);

  ingest->start();
  std::this_thread::sleep_for(400ms);
  ingest->stop();

  const std::vector<Frame> from_a = drain(a, 2s);
  const std::vector<Frame> from_b = drain(b, 2s);

  REQUIRE(from_a.size() > 100);
  REQUIRE(from_a.size() == from_b.size());
  CHECK(a.stats().dropped == 0);
  CHECK(b.stats().dropped == 0);

  bool seen[3] = {false, false, false};
  std::uint64_t last_per_source[3] = {0, 0, 0};
  bool have[3] = {false, false, false};

  for (std::size_t i = 0; i < from_a.size(); ++i) {
    // Two independent cursors over one copy must agree exactly.
    CHECK(from_a[i].seq == from_b[i].seq);
    CHECK(from_a[i].src_id == from_b[i].src_id);

    // The sequence is dense across all three producers: the ring, not any one
    // source, is the single point of ordering.
    if (i > 0) {
      REQUIRE(from_a[i].seq == from_a[i - 1].seq + 1);
    }

    const std::size_t p = from_a[i].src_id;
    REQUIRE(p < 3);
    seen[p] = true;

    // Per-source FIFO: within one bus, ingest order is preserved even though
    // three threads were claiming tickets concurrently.
    if (have[p]) {
      REQUIRE(from_a[i].t_ingest_ns >= last_per_source[p]);
    }
    last_per_source[p] = from_a[i].t_ingest_ns;
    have[p] = true;
  }

  for (bool s : seen) {
    CHECK(s);
  }
  CHECK(ingest->stats().frames == from_a.size());
}

TEST_CASE("egress serves a consumer from either topology", "[feed][egress]") {
  // Egress never learns which topology it is attached to. This runs the same
  // socket path over both.
  auto run = [](ConsumerFeed& feed, const std::function<void()>& publish) {
    std::string error;
    auto egress = Egress::create("test", feed, 0, error);
    REQUIRE(egress != nullptr);
    egress->start();

    auto stream = FrameStream::connect("127.0.0.1", egress->port(), error);
    REQUIRE(stream != nullptr);

    publish();

    std::vector<Frame> got;
    std::vector<Frame> batch;
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (got.size() < 200 && std::chrono::steady_clock::now() < until) {
      const FrameStream::Status st = stream->read_batch(batch, 100);
      if (st == FrameStream::Status::kClosed || st == FrameStream::Status::kProtocolError) {
        break;
      }
      got.insert(got.end(), batch.begin(), batch.end());
    }
    egress->stop();

    REQUIRE(got.size() == 200);
    for (std::size_t i = 0; i < got.size(); ++i) {
      CHECK(got[i].t_ingest_ns == i);
    }
  };

  std::vector<Frame> batch;
  for (std::uint64_t i = 0; i < 200; ++i) {
    batch.push_back(make_frame(i));
  }

  SECTION("queue") {
    QueueFeed feed{4096};
    run(feed, [&] {
      static_cast<void>(feed.queue().push_batch(std::span<const Frame>{batch}));
    });
  }
  SECTION("ring") {
    BroadcastRing ring{4096};
    RingFeed feed{ring};
    run(feed, [&] { ring.publish_batch(std::span<const Frame>{batch}); });
  }
}
