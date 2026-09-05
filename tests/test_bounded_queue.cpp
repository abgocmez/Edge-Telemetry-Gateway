#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <vector>

#include "bounded_queue.hpp"

using namespace etg;
using namespace std::chrono_literals;

namespace {

std::size_t push_one(BoundedQueue<int>& q, int v) {
  const int items[1] = {v};
  return q.push_batch(std::span<const int>{items, 1});
}

}  // namespace

TEST_CASE("items come out in the order they went in", "[queue]") {
  BoundedQueue<int> q{16};
  const std::vector<int> in{1, 2, 3, 4, 5};
  CHECK(q.push_batch(std::span<const int>{in}) == 0);

  std::vector<int> out(5);
  CHECK(q.pop_batch(std::span<int>{out}, 100ms) == 5);
  CHECK(out == in);
}

TEST_CASE("a full queue drops the oldest and keeps the newest", "[queue]") {
  BoundedQueue<int> q{4};
  const std::vector<int> in{1, 2, 3, 4, 5, 6};
  const std::size_t dropped = q.push_batch(std::span<const int>{in});

  CHECK(dropped == 2);

  std::vector<int> out(4);
  REQUIRE(q.pop_batch(std::span<int>{out}, 100ms) == 4);
  // The two oldest are gone; the four newest survived, still in order.
  CHECK(out == std::vector<int>{3, 4, 5, 6});
}

TEST_CASE("the accounting balances exactly", "[queue]") {
  BoundedQueue<int> q{8};
  for (int i = 0; i < 100; ++i) {
    push_one(q, i);
  }
  std::vector<int> out(3);
  const std::size_t got = q.pop_batch(std::span<int>{out}, 100ms);

  const QueueStats s = q.stats();
  CHECK(s.pushed == 100);
  CHECK(s.popped == got);
  // Nothing may vanish unaccounted for: everything offered was either handed
  // over, dropped on purpose, or is still sitting in the queue.
  CHECK(s.pushed == s.popped + s.dropped + s.size);
}

TEST_CASE("push never blocks even when nothing is consuming", "[queue]") {
  BoundedQueue<int> q{4};
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 10'000; ++i) {
    push_one(q, i);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  // A producer that waited for space would stall the whole pipeline behind one
  // slow consumer, which is the failure this design exists to avoid.
  CHECK(elapsed < 2s);
  CHECK(q.stats().dropped == 10'000 - 4);
}

TEST_CASE("pop times out rather than waiting forever on an empty queue", "[queue]") {
  BoundedQueue<int> q{4};
  std::vector<int> out(4);
  const auto start = std::chrono::steady_clock::now();
  CHECK(q.pop_batch(std::span<int>{out}, 50ms) == 0);
  CHECK(std::chrono::steady_clock::now() - start >= 40ms);
}

TEST_CASE("close wakes a consumer that is already blocked", "[queue]") {
  BoundedQueue<int> q{4};
  std::atomic<bool> returned{false};

  std::thread consumer([&] {
    std::vector<int> out(4);
    static_cast<void>(q.pop_batch(std::span<int>{out}, 10s));
    returned.store(true);
  });

  std::this_thread::sleep_for(50ms);
  CHECK_FALSE(returned.load());

  const auto start = std::chrono::steady_clock::now();
  q.close();
  consumer.join();

  CHECK(returned.load());
  // Woken by close, not by the 10s timeout expiring.
  CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("close is idempotent and refuses further pushes", "[queue]") {
  BoundedQueue<int> q{4};
  q.close();
  q.close();
  CHECK(q.closed());
  CHECK(push_one(q, 1) == 0);
  CHECK(q.stats().size == 0);
}

TEST_CASE("a producer and a consumer running concurrently lose nothing unaccounted for",
          "[queue][stress]") {
  constexpr int kItems = 200'000;
  BoundedQueue<int> q{1024};

  std::vector<int> received;
  received.reserve(kItems);

  std::thread consumer([&] {
    std::vector<int> buf(128);
    for (;;) {
      const std::size_t n = q.pop_batch(std::span<int>{buf}, 50ms);
      for (std::size_t i = 0; i < n; ++i) {
        received.push_back(buf[i]);
      }
      if (n == 0 && q.closed()) {
        // One last sweep, so a batch pushed just before close is not stranded.
        const std::size_t tail = q.pop_batch(std::span<int>{buf}, 0ms);
        for (std::size_t i = 0; i < tail; ++i) {
          received.push_back(buf[i]);
        }
        if (tail == 0) {
          return;
        }
      }
    }
  });

  for (int i = 0; i < kItems; ++i) {
    push_one(q, i);
  }
  q.close();
  consumer.join();

  const QueueStats s = q.stats();
  CHECK(s.pushed == kItems);
  CHECK(s.pushed == s.popped + s.dropped + s.size);
  CHECK(received.size() == s.popped);

  // Drops are permitted - the queue is deliberately far too small - but what
  // does arrive must be strictly increasing. A duplicate or a reordering would
  // mean the ring arithmetic is wrong under contention, which is exactly the
  // class of bug a single-threaded test cannot see.
  for (std::size_t i = 1; i < received.size(); ++i) {
    REQUIRE(received[i] > received[i - 1]);
  }
}
