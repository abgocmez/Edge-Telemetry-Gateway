#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <vector>

#include "broadcast_ring.hpp"

using namespace etg;
using namespace std::chrono_literals;

namespace {

constexpr std::uint64_t kTearKey = 0x5A5AA5A5DEADBEEFULL;

// Stress volumes are cut hard under a sanitizer.
//
// ThreadSanitizer instruments every atomic access, which turns the claim retry
// loop into a convoy: the collision test ran for over ten minutes under TSan
// against forty milliseconds without it. Cutting the volume is not weakening the
// test - the sanitized run exists to check that TSan sees these code paths at
// all, and the full-volume contention run happens in the unsanitized build,
// where it is fast enough to be worth doing properly.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr std::uint64_t kStressDivisor = 100;
constexpr unsigned kMaxStressThreads = 4;
#else
constexpr std::uint64_t kStressDivisor = 1;
constexpr unsigned kMaxStressThreads = 16;
#endif

// Every byte of the frame is derived from one counter, so any mixture of two
// frames fails the check below. Without this a torn read would be invisible:
// the sequence number would be plausible and only the payload would be wrong.
Frame make_frame(std::uint8_t producer, std::uint64_t counter) {
  Frame f{};
  f.seq = 0;  // the ring assigns this
  f.t_ingest_ns = counter;
  f.t_kernel_ns = counter ^ kTearKey;
  f.can_id = static_cast<std::uint32_t>(counter & 0x7FFU);
  f.src_id = producer;
  f.len = 8;
  f.flags = 0;
  f.reserved = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    f.data[i] = static_cast<std::uint8_t>((counter >> (8U * i)) & 0xFFU);
  }
  return f;
}

// Returns the counter the frame claims, or nullopt if the frame is internally
// inconsistent - which can only happen if two writers' bytes were mixed.
bool frame_is_intact(const Frame& f, std::uint64_t& counter) {
  counter = f.t_ingest_ns;
  if (f.t_kernel_ns != (counter ^ kTearKey)) {
    return false;
  }
  if (f.can_id != static_cast<std::uint32_t>(counter & 0x7FFU)) {
    return false;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    if (f.data[i] != static_cast<std::uint8_t>((counter >> (8U * i)) & 0xFFU)) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("a cell is exactly one cache line", "[ring]") {
  CHECK(sizeof(BroadcastRing::Cell) == 64);
  CHECK(alignof(BroadcastRing::Cell) == 64);
  CHECK(BroadcastRing::kFrameWords == 5);
}

TEST_CASE("capacity is rounded up to a power of two", "[ring]") {
  CHECK(BroadcastRing{1}.capacity() == 2);
  CHECK(BroadcastRing{8}.capacity() == 8);
  CHECK(BroadcastRing{9}.capacity() == 16);
  CHECK(BroadcastRing{1000}.capacity() == 1024);
}

TEST_CASE("the ring assigns the sequence, not the producer", "[ring]") {
  BroadcastRing ring{16};
  Frame f = make_frame(1, 42);
  f.seq = 99999;  // whatever a caller put here is not authoritative

  CHECK(ring.publish(f) == 0);
  CHECK(ring.publish(f) == 1);

  BroadcastRing::Cursor c;
  Frame out{};
  REQUIRE(ring.try_read(c, out) == BroadcastRing::ReadStatus::kOk);
  CHECK(out.seq == 0);  // the ticket, not 99999
  REQUIRE(ring.try_read(c, out) == BroadcastRing::ReadStatus::kOk);
  CHECK(out.seq == 1);
}

TEST_CASE("an empty ring reports empty rather than garbage", "[ring]") {
  BroadcastRing ring{16};
  BroadcastRing::Cursor c;
  Frame out{};
  CHECK(ring.try_read(c, out) == BroadcastRing::ReadStatus::kEmpty);
  CHECK(c.next == 0);
  CHECK(c.missed == 0);
}

TEST_CASE("a reader that keeps up sees every frame in order", "[ring]") {
  BroadcastRing ring{64};
  BroadcastRing::Cursor c;
  Frame out{};

  for (std::uint64_t i = 0; i < 500; ++i) {
    ring.publish(make_frame(1, i));
    REQUIRE(ring.try_read(c, out) == BroadcastRing::ReadStatus::kOk);
    CHECK(out.seq == i);

    std::uint64_t counter = 0;
    REQUIRE(frame_is_intact(out, counter));
    CHECK(counter == i);
  }
  CHECK(c.missed == 0);
}

TEST_CASE("a reader that falls behind is told exactly what it missed", "[ring]") {
  BroadcastRing ring{16};
  BroadcastRing::Cursor c;

  for (std::uint64_t i = 0; i < 100; ++i) {
    ring.publish(make_frame(1, i));
  }

  std::vector<Frame> got(64);
  const BroadcastRing::ReadResult r = ring.read_batch(c, got);

  // 100 published into 16 cells: the last 16 survive, the first 84 are gone and
  // must be reported rather than silently absent.
  CHECK(r.count == 16);
  CHECK(r.missed == 84);
  CHECK(got[0].seq == 84);
  CHECK(got[15].seq == 99);
  CHECK(c.next == 100);
}

TEST_CASE("the accounting balances: published equals received plus missed", "[ring]") {
  BroadcastRing ring{32};
  BroadcastRing::Cursor c;

  // Three read per round against seven published: the reader must fall behind.
  // An earlier version of this test allowed eight, so the reader kept up, no
  // lapping ever happened, and the interesting half of the invariant was never
  // exercised - the guard at the end is there to catch exactly that.
  std::vector<Frame> buf(3);

  std::uint64_t published = 0;
  std::uint64_t received = 0;

  for (int round = 0; round < 200; ++round) {
    for (int i = 0; i < 7; ++i) {
      ring.publish(make_frame(1, published++));
    }
    const BroadcastRing::ReadResult r = ring.read_batch(c, buf);
    received += r.count;
  }

  // Nothing may vanish unaccounted for. Frames still sitting in the ring are
  // neither received nor missed yet, which is why this is an inequality on the
  // remainder rather than an equality.
  const std::uint64_t still_readable = published - c.next;
  CHECK(received + c.missed + still_readable == published);
  CHECK(c.missed > 0);  // the test would be vacuous otherwise
}

TEST_CASE("attach starts at the oldest frame still present", "[ring]") {
  BroadcastRing ring{16};
  for (std::uint64_t i = 0; i < 100; ++i) {
    ring.publish(make_frame(1, i));
  }

  BroadcastRing::Cursor c = ring.attach();
  CHECK(c.next == 84);

  std::vector<Frame> got(32);
  const BroadcastRing::ReadResult r = ring.read_batch(c, got);
  CHECK(r.count == 16);
  // A consumer joining a running gateway starts from live data and is not told
  // it "missed" everything published before it existed.
  CHECK(r.missed == 0);
  CHECK(got[0].seq == 84);
}

TEST_CASE("a batch publish assigns contiguous sequences", "[ring]") {
  BroadcastRing ring{64};
  std::vector<Frame> batch;
  for (std::uint64_t i = 0; i < 10; ++i) {
    batch.push_back(make_frame(2, i));
  }

  CHECK(ring.publish_batch(batch) == 0);
  CHECK(ring.publish_batch(batch) == 10);

  BroadcastRing::Cursor c;
  std::vector<Frame> got(20);
  const BroadcastRing::ReadResult r = ring.read_batch(c, got);
  REQUIRE(r.count == 20);
  for (std::size_t i = 0; i < 20; ++i) {
    CHECK(got[i].seq == i);
  }
}

TEST_CASE("many producers never tear a frame and never lose per-producer order",
          "[ring][stress]") {
  // The central test. Four producers contending on a ring small enough that
  // cells are reused constantly, one reader deliberately slower than the
  // producers so lapping and mid-write collisions both happen.
  constexpr int kProducers = 4;
  const std::uint64_t kPerProducer = 40'000 / kStressDivisor;

  BroadcastRing ring{256};
  std::atomic<bool> go{false};
  std::atomic<int> finished{0};

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&ring, &go, &finished, kPerProducer, p] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (std::uint64_t i = 0; i < kPerProducer; ++i) {
        ring.publish(make_frame(static_cast<std::uint8_t>(p), i));
      }
      finished.fetch_add(1, std::memory_order_release);
    });
  }

  std::array<std::uint64_t, kProducers> last_seen{};
  std::array<bool, kProducers> seen_any{};
  std::uint64_t received = 0;
  std::uint64_t torn = 0;
  std::uint64_t out_of_order = 0;
  std::uint64_t last_seq = 0;
  bool have_seq = false;
  std::uint64_t seq_regressions = 0;

  std::thread reader([&] {
    BroadcastRing::Cursor c;
    std::vector<Frame> buf(32);
    while (finished.load(std::memory_order_acquire) < kProducers ||
           c.next < ring.write_position()) {
      const BroadcastRing::ReadResult r = ring.read_batch(c, buf);
      for (std::size_t i = 0; i < r.count; ++i) {
        const Frame& f = buf[i];
        ++received;

        std::uint64_t counter = 0;
        if (!frame_is_intact(f, counter)) {
          ++torn;
          continue;
        }

        // A reader's own sequence must never go backwards, lapped or not.
        if (have_seq && f.seq <= last_seq) {
          ++seq_regressions;
        }
        last_seq = f.seq;
        have_seq = true;

        const std::size_t p = f.src_id;
        if (p < kProducers) {
          if (seen_any[p] && counter <= last_seen[p]) {
            ++out_of_order;
          }
          last_seen[p] = counter;
          seen_any[p] = true;
        }
      }
      if (r.count == 0) {
        std::this_thread::yield();
      }
    }
  });

  go.store(true, std::memory_order_release);
  for (auto& t : producers) {
    t.join();
  }
  reader.join();

  // A torn frame is a correctness failure, full stop: it means two producers
  // interleaved their bytes into one cell and the sequence check did not catch
  // it. This is what the CAS claim exists to prevent.
  CHECK(torn == 0);

  // Loss is expected and fine - the ring is far too small on purpose - but what
  // does arrive must be in order. Out-of-order would mean the ticket ordering
  // or the cell protocol is broken under contention, which no single-threaded
  // test can see.
  CHECK(out_of_order == 0);
  CHECK(seq_regressions == 0);

  CHECK(ring.write_position() == static_cast<std::uint64_t>(kProducers) * kPerProducer);
  CHECK(received > 0);
  for (int p = 0; p < kProducers; ++p) {
    CHECK(seen_any[p]);  // every producer's work reached the reader at least once
  }
}

TEST_CASE("every reader sees the same stream independently", "[ring][stress]") {
  const std::uint64_t kFrames = 20'000 / kStressDivisor;
  constexpr int kReaders = 3;

  // Sized so nothing is lost: this test is about readers not interfering with
  // each other, and loss would make the comparison meaningless.
  BroadcastRing ring{1 << 15U};

  std::array<std::vector<std::uint64_t>, kReaders> seen;
  std::atomic<bool> done{false};

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      BroadcastRing::Cursor c;
      std::vector<Frame> buf(64);
      while (!done.load(std::memory_order_acquire) || c.next < ring.write_position()) {
        const BroadcastRing::ReadResult res = ring.read_batch(c, buf);
        for (std::size_t i = 0; i < res.count; ++i) {
          seen[r].push_back(buf[i].seq);
        }
        if (res.count == 0) {
          std::this_thread::yield();
        }
      }
      CHECK(c.missed == 0);
    });
  }

  for (std::uint64_t i = 0; i < kFrames; ++i) {
    ring.publish(make_frame(0, i));
  }
  done.store(true, std::memory_order_release);
  for (auto& t : readers) {
    t.join();
  }

  for (int r = 0; r < kReaders; ++r) {
    REQUIRE(seen[r].size() == kFrames);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
      REQUIRE(seen[r][i] == i);
    }
  }
}

TEST_CASE("a stalled reader does not stop producers", "[ring][stress]") {
  BroadcastRing ring{64};

  // A reader that reads one frame and then does nothing at all. If readers could
  // block writers, the producer below would stop after 64 frames.
  BroadcastRing::Cursor stalled;
  Frame ignored{};
  ring.publish(make_frame(1, 0));
  REQUIRE(ring.try_read(stalled, ignored) == BroadcastRing::ReadStatus::kOk);

  const auto begin = std::chrono::steady_clock::now();
  for (std::uint64_t i = 1; i < 100'000; ++i) {
    ring.publish(make_frame(1, i));
  }
  const auto elapsed = std::chrono::steady_clock::now() - begin;

  CHECK(ring.write_position() == 100'000);
  CHECK(elapsed < 5s);

  // The stalled reader discovers its loss only when it next looks, which is the
  // whole bargain: it costs the reader, never the producer.
  const BroadcastRing::ReadStatus st = ring.try_read(stalled, ignored);
  CHECK(st == BroadcastRing::ReadStatus::kLapped);
  CHECK(stalled.missed > 0);
}

TEST_CASE("producer collisions happen constantly, and the claim absorbs them",
          "[ring][stress]") {
  // The evidence for the CAS claim, rather than an argument for it.
  //
  // Two things were assumed here and both turned out to be wrong, which is why
  // the parameters are what they are.
  //
  // First, the collision was assumed to be rare. It is not: eight producers on a
  // sixteen-cell ring retry the claim tens of thousands of times in twenty
  // milliseconds. A producer holding ticket P cannot write until the holder of
  // P-capacity has published, and with tickets handed out by fetch_add the later
  // thread routinely runs ahead of the earlier one. The retry count is really a
  // measurement of that convoy.
  //
  // Second, "no torn frames" was assumed to be evidence on its own. It is not.
  // With the claim replaced by a plain store, the general 256-cell stress test
  // still passed - the tear needs a producer to publish *after* the producer
  // that lapped it, and on a large ring the reader has moved past that position
  // long before. Measured across a sweep: at 8 producers and 16 cells the broken
  // version tears twice in 400k publishes, and at 64 cells or more it tears not
  // at all. A test that cannot fail proves nothing, so this one asserts both
  // halves - that collisions occurred, and that none of them tore.
  //
  // Capacity 8 with four times the core count was tried first and ran for over
  // ten minutes: every producer waits on a cell eight positions back, the retry
  // loop serialises, and the test stops measuring contention and starts
  // measuring livelock.
  const unsigned hw = std::thread::hardware_concurrency();
  const unsigned producers = std::max(4U, std::min(hw, kMaxStressThreads));
  const std::uint64_t kPerProducer = 50'000 / kStressDivisor;

  BroadcastRing ring{16};
  std::atomic<bool> go{false};
  std::atomic<std::uint64_t> torn{0};
  std::atomic<bool> stop_reader{false};

  std::vector<std::thread> writers;
  writers.reserve(producers);
  for (unsigned p = 0; p < producers; ++p) {
    writers.emplace_back([&ring, &go, kPerProducer, p] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (std::uint64_t i = 0; i < kPerProducer; ++i) {
        ring.publish(make_frame(static_cast<std::uint8_t>(p & 0xFFU), i));
      }
    });
  }

  std::thread reader([&] {
    BroadcastRing::Cursor c;
    Frame f{};
    while (!stop_reader.load(std::memory_order_acquire)) {
      if (ring.try_read(c, f) == BroadcastRing::ReadStatus::kOk) {
        std::uint64_t counter = 0;
        if (!frame_is_intact(f, counter)) {
          torn.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  });

  go.store(true, std::memory_order_release);
  for (auto& t : writers) {
    t.join();
  }
  stop_reader.store(true, std::memory_order_release);
  reader.join();

  INFO("producers: " << producers);
  INFO("claim retries: " << ring.claim_retries());
  INFO("frames published: " << ring.write_position());

  // The defended-against situation actually arose in this run - but only assert
  // that where it is meant to. Under a sanitizer the volume is cut by two orders
  // of magnitude and a low-core runner then publishes too few frames to collide
  // at all: CI reported 2000 frames and zero retries, which is a correct
  // outcome, not a regression. The non-vacuity claim belongs to the run that is
  // supposed to be non-vacuous.
  if constexpr (kStressDivisor == 1) {
    CHECK(ring.claim_retries() > 0);
  }

  // And not one of those collisions produced a frame made of two producers'
  // bytes. Remove the CAS in write_at and this is what eventually fails.
  CHECK(torn.load() == 0);
  CHECK(ring.write_position() == producers * kPerProducer);
}
