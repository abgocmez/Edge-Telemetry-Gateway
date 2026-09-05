#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "frame.hpp"

namespace etg {

struct QueueStats {
  std::uint64_t pushed = 0;   // offered by the producer
  std::uint64_t popped = 0;   // handed to the consumer
  std::uint64_t dropped = 0;  // overwritten because the consumer fell behind
  std::size_t size = 0;
  std::size_t capacity = 0;
};

// A bounded single-producer/single-consumer queue guarded by a mutex.
//
// This is deliberately the boring implementation. It is topology A in full - one
// queue per consumer, one copy per consumer - and it is the baseline the
// lock-free broadcast ring is measured against later. It is not a placeholder:
// if the ring turns out to be wrong or late, this is a defensible architecture
// on its own.
//
// The load-bearing property is that **push never blocks**. A producer that
// waited for space would let one slow consumer stall the fan-out thread and so
// the entire pipeline, which is precisely the failure this project exists to
// study. When the queue is full the oldest frame is overwritten and counted.
//
// Drop-oldest is chosen rather than drop-newest to match what an overwriting
// ring does by construction, so the two topologies can be compared on equal
// terms rather than on differing loss policies.
//
// Counters are atomic and written with relaxed ordering: they are advisory
// telemetry, never used to synchronise anything, and paying for ordering on
// them would be paying for a guarantee nothing consumes. The mutex already
// orders the data they describe.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity)
      : buf_(capacity == 0 ? 1 : capacity), capacity_(capacity == 0 ? 1 : capacity) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // Returns how many items were dropped to make room. Never blocks.
  std::size_t push_batch(std::span<const T> items) {
    if (items.empty()) {
      return 0;
    }
    std::size_t dropped = 0;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return 0;
      }
      for (const T& item : items) {
        if (count_ == capacity_) {
          head_ = (head_ + 1) % capacity_;
          --count_;
          ++dropped;
        }
        buf_[(head_ + count_) % capacity_] = item;
        ++count_;
      }
    }
    pushed_.fetch_add(items.size(), std::memory_order_relaxed);
    dropped_.fetch_add(dropped, std::memory_order_relaxed);
    cv_.notify_one();
    return dropped;
  }

  // Blocks until at least one item is available, the queue is closed, or the
  // timeout expires. Returns how many items were written to `out`.
  std::size_t pop_batch(std::span<T> out, std::chrono::milliseconds timeout) {
    if (out.empty()) {
      return 0;
    }
    std::size_t n = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!cv_.wait_for(lock, timeout, [this] { return count_ > 0 || closed_; })) {
        return 0;  // timed out
      }
      n = count_ < out.size() ? count_ : out.size();
      for (std::size_t i = 0; i < n; ++i) {
        out[i] = buf_[(head_ + i) % capacity_];
      }
      head_ = (head_ + n) % capacity_;
      count_ -= n;
    }
    popped_.fetch_add(n, std::memory_order_relaxed);
    return n;
  }

  // Wakes every waiter and refuses further pushes. Idempotent, so shutdown does
  // not have to be raced or ordered by the caller.
  void close() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  [[nodiscard]] QueueStats stats() const {
    QueueStats s;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      s.size = count_;
    }
    s.capacity = capacity_;
    s.pushed = pushed_.load(std::memory_order_relaxed);
    s.popped = popped_.load(std::memory_order_relaxed);
    s.dropped = dropped_.load(std::memory_order_relaxed);
    return s;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<T> buf_;
  std::size_t capacity_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  bool closed_ = false;

  std::atomic<std::uint64_t> pushed_{0};
  std::atomic<std::uint64_t> popped_{0};
  std::atomic<std::uint64_t> dropped_{0};
};

// The queue this project actually instantiates. Declared beside the template
// rather than beside one of its users, so both topologies can name it without
// either having to include the other.
using FrameQueue = BoundedQueue<Frame>;

}  // namespace etg
