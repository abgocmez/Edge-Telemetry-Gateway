#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "frame.hpp"

namespace etg {

// Counters are per source and separated by cause. "Frames were lost" is not a
// useful thing to report; which layer lost them is.
struct SourceStats {
  std::uint64_t frames = 0;        // delivered to the caller
  std::uint64_t kernel_drops = 0;  // dropped by the kernel before we ever saw them
  std::uint64_t read_errors = 0;   // recvmmsg failures that were not EAGAIN
  std::uint64_t malformed = 0;     // received but unusable
};

// A thing that produces frames, expressed as a file descriptor plus a batch
// drain. The gateway owns the threads; a source never spawns one.
//
// This shape is not an invented abstraction. A CAN raw socket, the hidraw node
// of the LIN hardware, and a timerfd-driven synthetic generator are all
// genuinely file descriptors, which is what lets the threading topology change
// (thread-per-source versus one epoll thread over all of them) without any
// source implementation being touched. It also makes shutdown uniform: an
// eventfd in the epoll set wakes everything.
//
// Batch reads stay inside the source because recvmmsg is specific to sockets
// and hidraw has no equivalent.
//
// Virtual dispatch is deliberate: one indirect call per *batch*, not per frame,
// which puts it below measurement noise at any useful batch size. A templated
// design would force every caller into a header for no measurable gain.
class Source {
 public:
  virtual ~Source() = default;

  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;
  Source(Source&&) = delete;
  Source& operator=(Source&&) = delete;

  // Readable-when-frames-are-available descriptor, for poll or epoll.
  [[nodiscard]] virtual int fd() const noexcept = 0;

  // Identifies this bus. Per-source FIFO ordering is guaranteed within one id.
  [[nodiscard]] virtual std::uint8_t id() const noexcept = 0;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  // Non-blocking. Returns how many frames were written to `out`, 0 if none are
  // ready. Fills every field except `seq`, which is assigned later at
  // ring-claim time because a single source cannot know the global order.
  [[nodiscard]] virtual std::size_t drain(std::span<Frame> out) noexcept = 0;

  [[nodiscard]] virtual const SourceStats& stats() const noexcept = 0;

 protected:
  Source() = default;
};

// CLOCK_MONOTONIC in nanoseconds. The local latency domain; never subtract a
// CLOCK_REALTIME value from one of these.
[[nodiscard]] std::uint64_t monotonic_ns() noexcept;

// A consumer's latency sample is (its own CLOCK_MONOTONIC now) minus the
// gateway's t_ingest_ns, and that is a delay only while both readings come from
// the same clock. Run the consumer on a second machine and each clock counts
// from its own boot, so the subtraction returns the difference in uptime
// instead: measured between this project's Pi and its development machine, it
// came to minus 3383 seconds. Rendered in microseconds, a dashboard would have
// shown that as an authoritative-looking latency panel.
//
// Separating the two cases is cheap, because a delay is never negative and a
// real one is never seconds long. A consumer that finds itself outside this
// window is not slow, it is holding a different clock, and should say so rather
// than publish the number.
constexpr std::int64_t kMaxPlausibleLatencyNs = 10'000'000'000;

[[nodiscard]] constexpr bool same_clock_domain(std::int64_t delay_ns) noexcept {
  return delay_ns >= 0 && delay_ns <= kMaxPlausibleLatencyNs;
}

// CLOCK_REALTIME in nanoseconds. The domain chrony disciplines and the domain
// SO_TIMESTAMPING reports in.
[[nodiscard]] std::uint64_t realtime_ns() noexcept;

}  // namespace etg
