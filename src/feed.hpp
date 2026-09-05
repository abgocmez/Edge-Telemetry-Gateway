#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "bounded_queue.hpp"
#include "broadcast_ring.hpp"
#include "frame.hpp"

namespace etg {

// What an egress thread needs from whatever is feeding it, and nothing else.
//
// Two implementations sit behind this: a per-consumer mutex queue (topology A,
// N copies of every frame) and a cursor into the shared lock-free broadcast ring
// (topology B, one copy read by everyone). Egress cannot tell them apart, which
// is the point - the two can be swapped at runtime and measured against each
// other on identical work, rather than compared as "the old version" against
// "the new version" with everything else different too.
//
// One virtual call per batch, not per frame; the same reasoning as the Source
// interface, and the same conclusion.

struct FeedStats {
  std::uint64_t delivered = 0;  // frames handed to this consumer
  std::uint64_t dropped = 0;    // frames this consumer lost, and knows it lost
  std::size_t backlog = 0;      // frames waiting for it right now
  std::size_t capacity = 0;
};

class ConsumerFeed {
 public:
  virtual ~ConsumerFeed() = default;

  ConsumerFeed(const ConsumerFeed&) = delete;
  ConsumerFeed& operator=(const ConsumerFeed&) = delete;

  // Blocks until at least one frame is available, the feed is closed, or the
  // timeout expires. Returns how many frames were written to `out`.
  virtual std::size_t pop_batch(std::span<Frame> out, std::chrono::milliseconds timeout) = 0;

  // Wakes anything parked in pop_batch and makes further calls return promptly.
  virtual void close() = 0;
  [[nodiscard]] virtual bool closed() const = 0;

  [[nodiscard]] virtual FeedStats stats() const = 0;
  [[nodiscard]] virtual const char* topology() const noexcept = 0;

 protected:
  ConsumerFeed() = default;
};

// Topology A: this consumer's own queue, written by the fan-out thread.
//
// Every consumer costs one copy of every frame, and its loss is entirely its
// own because nothing is shared.
class QueueFeed final : public ConsumerFeed {
 public:
  explicit QueueFeed(std::size_t capacity) : queue_(capacity) {}

  std::size_t pop_batch(std::span<Frame> out, std::chrono::milliseconds timeout) override {
    return queue_.pop_batch(out, timeout);
  }

  void close() override { queue_.close(); }
  [[nodiscard]] bool closed() const override { return queue_.closed(); }
  [[nodiscard]] const char* topology() const noexcept override { return "queue"; }

  [[nodiscard]] FeedStats stats() const override {
    const QueueStats q = queue_.stats();
    FeedStats s;
    s.delivered = q.popped;
    s.dropped = q.dropped;
    s.backlog = q.size;
    s.capacity = q.capacity;
    return s;
  }

  [[nodiscard]] FrameQueue& queue() noexcept { return queue_; }

 private:
  FrameQueue queue_;
};

// Topology B: a private cursor into the shared ring.
//
// Nothing is copied per consumer and nothing is written per consumer; the ring
// holds one copy and every cursor reads it in place. A consumer that falls
// behind is overwritten and finds out from the sequence number, which is why
// `dropped` here is a count the reader derives rather than one somebody else
// recorded on its behalf.
class RingFeed final : public ConsumerFeed {
 public:
  // Attaches at the oldest frame still present rather than at zero, so a
  // consumer joining a running gateway starts on live data instead of being
  // told it missed everything published before it existed.
  explicit RingFeed(BroadcastRing& ring) : ring_(ring), cursor_(ring.attach()) {}

  std::size_t pop_batch(std::span<Frame> out, std::chrono::milliseconds timeout) override {
    BroadcastRing::ReadResult r = ring_.read_batch(cursor_, out);
    if (r.count > 0) {
      publish_stats(r.count);
      return r.count;
    }
    if (ring_.closed()) {
      return 0;
    }

    // Nothing ready: park rather than spin. On a busy feed this is never
    // reached, and on an idle one it is the difference between a sleeping
    // thread and a burnt core.
    ring_.wait_for_data(cursor_.next, timeout);

    r = ring_.read_batch(cursor_, out);
    publish_stats(r.count);
    return r.count;
  }

  void close() override { ring_.close(); }
  [[nodiscard]] bool closed() const override { return ring_.closed(); }
  [[nodiscard]] const char* topology() const noexcept override { return "ring"; }

  [[nodiscard]] FeedStats stats() const override {
    FeedStats s;
    s.delivered = delivered_.load(std::memory_order_relaxed);
    s.dropped = missed_.load(std::memory_order_relaxed);
    const std::uint64_t pos = position_.load(std::memory_order_relaxed);
    const std::uint64_t w = ring_.write_position();
    s.backlog = w > pos ? static_cast<std::size_t>(w - pos) : 0;
    s.capacity = ring_.capacity();
    return s;
  }

 private:
  // The cursor belongs to the egress thread alone and is deliberately not
  // atomic - readers not coordinating with anyone is the whole design. But
  // stats() is called from another thread, so the numbers it reports are
  // mirrored into relaxed atomics instead of being read out of the cursor
  // directly. Reading the cursor from the reporting thread would be a data
  // race, and one TSan would have found the moment a stats line was printed
  // while traffic was flowing.
  void publish_stats(std::size_t count) {
    delivered_.fetch_add(count, std::memory_order_relaxed);
    missed_.store(cursor_.missed, std::memory_order_relaxed);
    position_.store(cursor_.next, std::memory_order_relaxed);
  }

  BroadcastRing& ring_;
  BroadcastRing::Cursor cursor_;

  std::atomic<std::uint64_t> delivered_{0};
  std::atomic<std::uint64_t> missed_{0};
  std::atomic<std::uint64_t> position_{0};
};

}  // namespace etg
