#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "bounded_queue.hpp"
#include "frame.hpp"
#include "source.hpp"

namespace etg {

// The ingest thread: drains one source, stamps each frame with the global
// sequence number, and copies it into every sink.
//
// This is where `seq` finally comes from. A source cannot assign it because a
// single bus does not know the global order; in M1 this thread is the single
// point of ordering, which is the direct analogue of the ticket a producer
// claims from the ring later.
//
// Copying into N sinks is topology A: N copies per frame, complete isolation
// between consumers. It is the cost the broadcast ring is meant to remove, and
// keeping it honest here is what makes that later comparison mean something.
//
// Shutdown is a designed feature rather than an afterthought. An eventfd sits
// in the poll set beside the sources, so stop() wakes the thread immediately
// instead of waiting out a poll timeout, and the thread never has to test a
// flag on a hot path.
//
// Several sources are polled by this *one* thread, and that is forced rather
// than chosen. Ordering here comes from the order frames are pushed, not from
// the sequence number, so two ingest threads taking sequences 5 and 6 could push
// 6 first and hand a consumer an out-of-order stream. Topology A therefore has a
// single serialisation point at ingest. The ring does not: its order comes from
// the cell position, so it takes M producers without one. That difference is a
// large part of what the two topologies are being measured for.
class FanOut {
 public:
  struct Stats {
    std::uint64_t frames = 0;   // frames read from the source and fanned out
    std::uint64_t batches = 0;  // drain calls that returned at least one frame
    std::uint64_t poll_errors = 0;
  };

  static std::unique_ptr<FanOut> create(std::vector<Source*> sources,
                                        std::vector<FrameQueue*> sinks, std::string& error);

  // Single-source convenience; the common case and every existing caller.
  static std::unique_ptr<FanOut> create(Source& source, std::vector<FrameQueue*> sinks,
                                        std::string& error) {
    return create(std::vector<Source*>{&source}, std::move(sinks), error);
  }

  ~FanOut();

  FanOut(const FanOut&) = delete;
  FanOut& operator=(const FanOut&) = delete;

  void start();

  // Idempotent, and safe to call from any thread. Joins before returning.
  void stop();

  [[nodiscard]] Stats stats() const noexcept;

  // The sequence the next frame will be given. Only meaningful once stopped.
  [[nodiscard]] std::uint64_t next_seq() const noexcept {
    return next_seq_.load(std::memory_order_relaxed);
  }

 private:
  FanOut(std::vector<Source*> sources, std::vector<FrameQueue*> sinks, int stop_fd);

  void run();

  std::vector<Source*> sources_;
  std::vector<FrameQueue*> sinks_;
  int stop_fd_;
  std::thread thread_;
  bool started_ = false;

  std::atomic<std::uint64_t> next_seq_{0};
  std::atomic<std::uint64_t> frames_{0};
  std::atomic<std::uint64_t> batches_{0};
  std::atomic<std::uint64_t> poll_errors_{0};
};

}  // namespace etg
