#pragma once

#include <cstdint>

#include "frame.hpp"
#include "wire.hpp"

namespace etg {

// Consumer-side loss accounting, shared by every consumer so they all report
// the same thing in the same words.
//
// Wire version 2 makes loss explicit: the gateway sends a marker naming the
// first sequence a consumer did not get and how many are missing. Before that,
// a consumer could only infer loss from a jump in the sequence numbers, which
// meant it could see *that* frames were gone but never *why* - and could not
// tell a gap it caused from one that happened upstream of it.
//
// Both paths are still counted, and the difference between them matters:
//
//   marker    the gateway said so. Authoritative, with a reason attached.
//
//   silent    the sequence jumped and no marker preceded it. Under v2 this
//             should never happen, so it is not "loss detected the old way" -
//             it is evidence that the gateway failed to report something it
//             knew, or that a marker was itself lost. Counting it separately is
//             what makes the marker mechanism falsifiable instead of merely
//             present.
class GapTracker {
 public:
  struct Stats {
    std::uint64_t frames = 0;         // real frames, markers excluded
    std::uint64_t markers = 0;        // loss markers received
    std::uint64_t reported_lost = 0;  // frames those markers accounted for
    std::uint64_t silent_jumps = 0;   // sequence jumps with no marker
    std::uint64_t silent_lost = 0;    // frames those jumps accounted for
    std::uint64_t overrun_markers = 0;
    std::uint64_t ingest_markers = 0;
    std::uint64_t absent_markers = 0;   // this consumer was away; not a fault
    std::uint64_t absent_frames = 0;
    std::uint64_t echoes = 0;           // round-trip probes seen
  };

  // Returns true if this was a real frame, false if it was a marker. Callers
  // that measure latency or record payloads must skip the markers: a marker has
  // no bus timestamp and never existed on a wire.
  bool observe(const Frame& f) {
    // An echo carries a nonce in its sequence field, not a sequence. Letting it
    // through would make the very next real frame look like a jump of a few
    // billion.
    if (wire::is_echo(f)) {
      ++s_.echoes;
      return false;
    }
    if (wire::is_gap(f)) {
      const std::uint64_t n = wire::gap_count(f);
      const GapReason reason = wire::gap_reason(f);
      ++s_.markers;
      if (reason == GapReason::kConsumerAbsent) {
        // A discontinuity, not loss. Counting it as loss would make every
        // restart look like a fault and quietly inflate every drop figure.
        ++s_.absent_markers;
        s_.absent_frames += n;
      } else if (reason == GapReason::kIngestLoss) {
        ++s_.ingest_markers;
        s_.reported_lost += n;
      } else {
        ++s_.overrun_markers;
        s_.reported_lost += n;
      }
      // A marker names the first missing sequence and how many follow, so the
      // stream is expected to resume exactly here.
      next_ = f.seq + n;
      have_ = true;
      return false;
    }

    if (have_ && f.seq > next_) {
      ++s_.silent_jumps;
      s_.silent_lost += f.seq - next_;
    }
    next_ = f.seq + 1;
    have_ = true;
    ++s_.frames;
    return true;
  }

  // A consumer that reconnects has not lost anything it was entitled to; it
  // simply was not there. Call this on every reconnect or the first batch of
  // the new stream is reported as a huge gap.
  void reset() noexcept { have_ = false; }

  [[nodiscard]] const Stats& stats() const noexcept { return s_; }
  [[nodiscard]] std::uint64_t total_lost() const noexcept {
    return s_.reported_lost + s_.silent_lost;
  }

 private:
  Stats s_;
  std::uint64_t next_ = 0;
  bool have_ = false;
};

}  // namespace etg
