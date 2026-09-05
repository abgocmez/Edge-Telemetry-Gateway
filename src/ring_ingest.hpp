#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "broadcast_ring.hpp"
#include "source.hpp"

namespace etg {

// Topology B's ingest: one thread per source, all publishing into one ring.
//
// This is what makes the ring's multi-producer machinery real rather than
// theoretical. With a single bus there is one producer, the CAS claim never
// contends, and the whole design is a seqlock with extra steps. A gateway that
// taps several buses at once is also the realistic product - an edge gateway
// watching one bus is an odd thing to build.
//
// Thread-per-source is chosen here, not settled. The alternative is one epoll
// thread over every source, which trades M threads for M wakeups on one thread;
// on a four-core Pi with four buses that difference is worth measuring rather
// than assuming, and the Source interface was shaped so the swap touches nothing
// but this file.
//
// There is no fan-out thread and no per-consumer copy. A source thread writes a
// frame into the ring exactly once, and every consumer reads that one copy
// through its own cursor. That is the whole difference from topology A, and the
// reason the two are worth measuring against each other.
class RingIngest {
 public:
  struct Stats {
    std::uint64_t frames = 0;
    std::uint64_t batches = 0;
    std::uint64_t poll_errors = 0;
  };

  static std::unique_ptr<RingIngest> create(std::vector<Source*> sources, BroadcastRing& ring,
                                            std::string& error);

  ~RingIngest();

  RingIngest(const RingIngest&) = delete;
  RingIngest& operator=(const RingIngest&) = delete;

  void start();
  void stop();

  [[nodiscard]] Stats stats() const noexcept;
  [[nodiscard]] std::size_t source_count() const noexcept { return sources_.size(); }

 private:
  RingIngest(std::vector<Source*> sources, BroadcastRing& ring, int stop_fd);

  void run(Source& source);

  std::vector<Source*> sources_;
  BroadcastRing& ring_;
  int stop_fd_;
  std::vector<std::thread> threads_;
  bool started_ = false;

  std::atomic<std::uint64_t> frames_{0};
  std::atomic<std::uint64_t> batches_{0};
  std::atomic<std::uint64_t> poll_errors_{0};
};

}  // namespace etg
