#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fanout.hpp"
#include "synthetic_source.hpp"

using namespace etg;
using namespace std::chrono_literals;

namespace {

std::unique_ptr<SyntheticSource> fast_source(std::uint8_t id = 1) {
  SyntheticSource::Config cfg;
  cfg.src_id = id;
  cfg.period_ns = 200'000;  // 5 kHz
  cfg.frames_per_tick = 1;
  std::string error;
  auto src = SyntheticSource::open(cfg, error);
  REQUIRE(src != nullptr);
  return src;
}

std::vector<Frame> drain_all(FrameQueue& q) {
  std::vector<Frame> out;
  std::vector<Frame> buf(256);
  for (;;) {
    const std::size_t n = q.pop_batch(std::span<Frame>{buf}, 0ms);
    if (n == 0) {
      return out;
    }
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
  }
}

}  // namespace

TEST_CASE("fan-out refuses a configuration it cannot honour", "[fanout]") {
  auto src = fast_source();
  std::string error;

  SECTION("no sinks") {
    CHECK(FanOut::create(*src, {}, error) == nullptr);
    CHECK_FALSE(error.empty());
  }

  SECTION("a null sink") {
    CHECK(FanOut::create(*src, {nullptr}, error) == nullptr);
    CHECK_FALSE(error.empty());
  }
}

TEST_CASE("every sink receives the same frames in the same order", "[fanout]") {
  auto src = fast_source();
  FrameQueue a{4096};
  FrameQueue b{4096};

  std::string error;
  auto fan = FanOut::create(*src, {&a, &b}, error);
  REQUIRE(fan != nullptr);

  fan->start();
  std::this_thread::sleep_for(300ms);
  fan->stop();

  const std::vector<Frame> from_a = drain_all(a);
  const std::vector<Frame> from_b = drain_all(b);

  REQUIRE(from_a.size() > 10);
  REQUIRE(from_a.size() == from_b.size());
  CHECK(a.stats().dropped == 0);
  CHECK(b.stats().dropped == 0);

  for (std::size_t i = 0; i < from_a.size(); ++i) {
    CHECK(from_a[i].seq == from_b[i].seq);
    CHECK(from_a[i].can_id == from_b[i].can_id);
    CHECK(from_a[i].t_ingest_ns == from_b[i].t_ingest_ns);
  }
}

TEST_CASE("sequence numbers start at zero and have no gaps", "[fanout]") {
  auto src = fast_source();
  FrameQueue q{8192};

  std::string error;
  auto fan = FanOut::create(*src, {&q}, error);
  REQUIRE(fan != nullptr);

  fan->start();
  std::this_thread::sleep_for(300ms);
  fan->stop();

  const std::vector<Frame> frames = drain_all(q);
  REQUIRE(frames.size() > 10);

  // seq is the gateway's own numbering, not the source's. A gap here would
  // later be indistinguishable from real loss at a consumer, so it must be
  // dense at the point of assignment.
  CHECK(frames.front().seq == 0);
  for (std::size_t i = 1; i < frames.size(); ++i) {
    REQUIRE(frames[i].seq == frames[i - 1].seq + 1);
  }
  CHECK(fan->next_seq() == frames.size() + q.stats().size);
  CHECK(fan->stats().frames == frames.size());
}

TEST_CASE("a sink that nobody drains does not affect the other sink", "[fanout]") {
  auto src = fast_source();
  FrameQueue healthy{8192};
  FrameQueue neglected{8};  // deliberately tiny and never read

  std::string error;
  auto fan = FanOut::create(*src, {&healthy, &neglected}, error);
  REQUIRE(fan != nullptr);

  fan->start();
  std::this_thread::sleep_for(300ms);
  fan->stop();

  const std::vector<Frame> frames = drain_all(healthy);

  REQUIRE(frames.size() > 10);
  CHECK(healthy.stats().dropped == 0);
  // The neglected sink absorbs its own loss and reports it. This is the whole
  // point of the topology: one consumer falling behind is that consumer's
  // problem and nobody else's.
  CHECK(neglected.stats().dropped > 0);
  CHECK(neglected.stats().pushed == healthy.stats().pushed);

  for (std::size_t i = 1; i < frames.size(); ++i) {
    REQUIRE(frames[i].seq == frames[i - 1].seq + 1);
  }
}

TEST_CASE("stop is prompt and idempotent", "[fanout]") {
  auto src = fast_source();
  FrameQueue q{1024};

  std::string error;
  auto fan = FanOut::create(*src, {&q}, error);
  REQUIRE(fan != nullptr);

  fan->start();
  std::this_thread::sleep_for(50ms);

  const auto begin = std::chrono::steady_clock::now();
  fan->stop();
  const auto elapsed = std::chrono::steady_clock::now() - begin;

  // The eventfd in the poll set is what makes this immediate. Without it,
  // shutdown would have to wait out the poll timeout.
  CHECK(elapsed < 100ms);

  fan->stop();
  fan->stop();
  CHECK(fan->stats().poll_errors == 0);
}

TEST_CASE("destroying a running fan-out stops it", "[fanout]") {
  auto src = fast_source();
  FrameQueue q{1024};

  std::string error;
  {
    auto fan = FanOut::create(*src, {&q}, error);
    REQUIRE(fan != nullptr);
    fan->start();
    std::this_thread::sleep_for(50ms);
  }  // no explicit stop: the destructor must join, or this test hangs or aborts

  CHECK(q.stats().pushed > 0);
}

TEST_CASE("stats can be read while the fan-out thread is running", "[fanout][stress]") {
  auto src = fast_source();
  FrameQueue q{8192};

  std::string error;
  auto fan = FanOut::create(*src, {&q}, error);
  REQUIRE(fan != nullptr);

  fan->start();

  // A monitoring thread reading counters while ingest runs is the real usage,
  // and it is what makes the atomics load-bearing rather than decorative. With
  // a plain integer here TSan reports a data race on this exact pair.
  std::uint64_t last = 0;
  const auto until = std::chrono::steady_clock::now() + 250ms;
  int reads = 0;
  while (std::chrono::steady_clock::now() < until) {
    const FanOut::Stats s = fan->stats();
    REQUIRE(s.frames >= last);  // counters only ever move forward
    last = s.frames;
    static_cast<void>(q.stats());
    ++reads;
  }

  fan->stop();
  CHECK(reads > 0);
  CHECK(last > 0);
}
