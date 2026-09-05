#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <mutex>

#include "feed.hpp"
#include "samples.hpp"

namespace etg {

// One consumer's outbound path: its own queue, its own thread, its own TCP
// port, one connected client at a time.
//
// A port per consumer rather than one port with many clients, because it makes
// the mapping visible: this queue belongs to that consumer, and its drop count
// is that consumer's loss and nobody else's.
//
// Back-pressure lives here and is real rather than simulated. Writes are
// non-blocking, so a consumer that cannot keep up fills the kernel socket
// buffer, the write returns EWOULDBLOCK, this thread stops draining, and the
// queue overflows and drops - in that order. `would_block` is therefore the
// direct measurement of back-pressure, not a proxy for it.
//
// While no client is connected the queue is deliberately left to fill and drop
// rather than being drained and discarded. A consumer that reconnects then
// receives the freshest frames available, and the drop counter tells it exactly
// what it missed.
class Egress {
 public:
  // Frames per wire batch, and how long to wait to fill one.
  //
  // These are the two halves of the same trade. At raw passthrough there is
  // almost no CPU work per frame, so the cost is the write syscall: one per
  // frame at low rates, which is the dominant expense. Coalescing amortises it
  // and buys throughput; waiting to coalesce costs tail latency directly.
  //
  // A linger of zero sends whatever is available immediately, which is the
  // lowest-latency and least efficient setting. This is deliberately Nagle's
  // algorithm re-implemented where it can be measured and turned off, rather
  // than left to the kernel where TCP_NODELAY is the only control.
  static constexpr std::size_t kDefaultBatch = 256;
  static constexpr std::size_t kMaxBatch = 4096;

  // Egress tuning. Grouped because all three change what a consumer experiences
  // rather than what the gateway computes.
  struct Tuning {
    std::size_t max_frames = kDefaultBatch;
    std::uint32_t linger_us = 0;

    // How often to send a round-trip probe, 0 to disable.
    //
    // The probe is timed by this machine's clock from send to return, so it
    // needs no agreement with the consumer's clock at all - which is the point.
    // Measuring one-way latency to another host requires the two clocks to
    // agree, and on the pair this was built for they differ by 1.19 seconds
    // because the consumer's host has never completed an NTP exchange.
    //
    // It travels in-band, through the same batching and socket as the frames
    // around it, so it measures what a frame experiences rather than what an
    // idle side-channel would. Halving assumes a symmetric path, and the
    // consumer's turnaround is inside the figure; both are stated rather than
    // corrected for.
    std::uint32_t echo_ms = 0;
  };

  struct Stats {
    std::uint64_t frames_sent = 0;
    std::uint64_t batches_sent = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t would_block = 0;     // the back-pressure signal
    std::uint64_t partial_writes = 0;  // a write that took only part of the buffer
    std::uint64_t connects = 0;
    std::uint64_t disconnects = 0;
    std::uint64_t gaps_sent = 0;    // loss markers emitted to this consumer
    std::uint64_t frames_lost = 0;  // frames those markers accounted for
    std::uint64_t lingered = 0;     // batches that waited to coalesce
    std::uint64_t echoes_sent = 0;
    std::uint64_t echoes_returned = 0;
    std::uint64_t echoes_lost = 0;  // never came back before the next was due
    std::int64_t rtt_p50_ns = 0;
    std::int64_t rtt_p99_ns = 0;
    std::int64_t rtt_max_ns = 0;
    bool connected = false;
  };

  static std::unique_ptr<Egress> create(std::string name, ConsumerFeed& feed, std::uint16_t port,
                                        Tuning tuning, std::string& error);

  static std::unique_ptr<Egress> create(std::string name, ConsumerFeed& feed, std::uint16_t port,
                                        std::string& error) {
    return create(std::move(name), feed, port, Tuning{}, error);
  }

  ~Egress();

  Egress(const Egress&) = delete;
  Egress& operator=(const Egress&) = delete;

  void start();
  void stop();

  [[nodiscard]] Stats stats() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }

 private:
  Egress(std::string name, ConsumerFeed& feed, std::uint16_t port, Tuning tuning, int listen_fd,
         int stop_fd);

  // Injects a probe when one is due, and reads whatever the consumer has sent
  // back. This is the only place the gateway reads from a consumer at all.
  void maybe_send_echo();
  void drain_replies();

  // Waits up to the linger budget for more frames, so one write carries more
  // than one frame. Returns the total now in `batch`.
  std::size_t coalesce(std::vector<Frame>& batch, std::size_t have);

  void run();
  bool wait_for_client();          // returns false when asked to stop
  bool flush_pending();            // returns false when the client is gone
  bool client_gone();              // peer closed, seen without writing to it
  void encode_batch(std::span<const Frame> frames);

  // Detects loss from the sequence numbers themselves and prepends a marker.
  //
  // Done here rather than in the feed because it works identically for both
  // topologies: whether a mutex queue overwrote the oldest entry or a ring cell
  // was lapped, the consumer's next frame simply arrives with a higher sequence
  // than the one after the last it received. The egress is also the only place
  // that knows what this particular consumer actually got.
  void note_gap(std::span<const Frame> frames);
  void drop_client();

  std::string name_;
  ConsumerFeed& feed_;
  std::uint16_t port_;
  Tuning tuning_;
  int listen_fd_;
  int stop_fd_;
  int client_fd_ = -1;

  std::vector<std::byte> pending_;
  std::size_t pending_offset_ = 0;

  // Reused buffer so a batch that needs a marker prepended does not allocate on
  // the hot path.
  std::vector<Frame> outgoing_;

  // Deliberately *not* reset on disconnect. A consumer that reconnects and
  // finds the stream has moved on did not lose anything it was entitled to, but
  // it still needs to be told there is a hole - a recorder appending to a file
  // must not leave an unexplained jump in it. So the position is remembered
  // across the outage and the gap is reported with a reason that says "you were
  // away" rather than "you were too slow".
  std::uint64_t next_seq_ = 0;
  bool have_seq_ = false;
  bool reconnected_ = false;

  std::thread thread_;
  bool started_ = false;

  std::atomic<std::uint64_t> frames_sent_{0};
  std::atomic<std::uint64_t> batches_sent_{0};
  std::atomic<std::uint64_t> bytes_sent_{0};
  std::atomic<std::uint64_t> would_block_{0};
  std::atomic<std::uint64_t> partial_writes_{0};
  std::atomic<std::uint64_t> connects_{0};
  std::atomic<std::uint64_t> disconnects_{0};
  // Only one probe is outstanding at a time, which keeps the state a single
  // entry instead of a map and makes a lost probe self-limiting.
  std::uint64_t echo_nonce_ = 0;
  std::uint64_t echo_sent_ns_ = 0;
  bool echo_outstanding_ = false;
  std::uint64_t next_echo_ns_ = 0;
  std::vector<std::byte> reply_buf_;

  mutable std::mutex rtt_mutex_;
  Samples rtt_;

  std::atomic<std::uint64_t> echoes_sent_{0};
  std::atomic<std::uint64_t> echoes_returned_{0};
  std::atomic<std::uint64_t> echoes_lost_{0};
  std::atomic<std::uint64_t> lingered_{0};
  std::atomic<std::uint64_t> gaps_sent_{0};
  std::atomic<std::uint64_t> frames_lost_{0};
  std::atomic<bool> connected_{false};
};

}  // namespace etg
