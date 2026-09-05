#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "frame.hpp"
#include "wire.hpp"

namespace etg {

// Per-id cadence checking, separated from the binary so the claim it exists to
// make can be tested rather than only demonstrated.
//
// Periodic traffic is the normal shape of a CAN bus, so "this id stopped
// arriving on time" is a real fault signal that needs no knowledge of what the
// payloads mean. But it is unavoidably a measurement of inter-arrival time, and
// a consumer that lost frames sees a long interval that never happened: the bus
// was fine, the consumer was slow.
//
// Hence `gap_aware`. When the gateway reports loss, the marker says how many
// sequences went missing but not which ids they carried - so any id could have
// been among them, and the only sound response is to treat the next interval
// for *every* id as unmeasurable. Suppressing only the ids seen nearby would
// still let false alarms through for the quiet ones, which are exactly the ids
// this is watching.
class CadenceMonitor {
 public:
  struct IdStats {
    std::uint64_t frames = 0;
    std::uint64_t expected_ns = 0;
    std::uint64_t anomalies = 0;
    std::uint64_t suppressed = 0;
    std::uint64_t worst_ns = 0;
  };

  CadenceMonitor(std::size_t calibration_samples, double factor, bool gap_aware)
      : calibrate_(calibration_samples < 2 ? 2 : calibration_samples),
        factor_(factor < 1.0 ? 1.0 : factor),
        gap_aware_(gap_aware) {}

  // Feeds one record. Returns true if it was flagged as an anomaly.
  bool observe(const Frame& f) {
    if (wire::is_gap(f)) {
      ++markers_;
      reported_lost_ += wire::gap_count(f);
      if (gap_aware_) {
        for (auto& [id, s] : ids_) {
          s.resync = true;
        }
      }
      return false;
    }

    ++frames_;
    State& s = ids_[f.can_id];
    ++s.stats.frames;

    // The gateway's ingest timestamp, not local arrival: it is the closest thing
    // to when the frame was on the bus, and it keeps network jitter out of a
    // measurement that is supposed to be about the bus.
    const std::uint64_t now = f.t_ingest_ns;

    if (!s.have_last) {
      s.last_ns = now;
      s.have_last = true;
      return false;
    }

    const std::uint64_t dt = now > s.last_ns ? now - s.last_ns : 0;
    s.last_ns = now;

    if (s.resync) {
      s.resync = false;
      ++s.stats.suppressed;
      ++suppressed_;
      return false;
    }

    if (s.samples.size() < calibrate_) {
      s.samples.push_back(dt);
      if (s.samples.size() == calibrate_) {
        // Median rather than mean: one long interval during calibration would
        // otherwise inflate the expectation and hide real faults afterwards.
        std::vector<std::uint64_t> v = s.samples;
        std::sort(v.begin(), v.end());
        s.stats.expected_ns = v[v.size() / 2];
      }
      return false;
    }

    if (dt > static_cast<std::uint64_t>(factor_ * static_cast<double>(s.stats.expected_ns))) {
      ++s.stats.anomalies;
      ++anomalies_;
      s.stats.worst_ns = std::max(s.stats.worst_ns, dt);
      return true;
    }
    return false;
  }

  // A reconnect is a discontinuity for every id, for the same reason a gap is.
  void reset_all() {
    for (auto& [id, s] : ids_) {
      s.resync = true;
    }
  }

  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
  [[nodiscard]] std::uint64_t anomalies() const noexcept { return anomalies_; }
  [[nodiscard]] std::uint64_t suppressed() const noexcept { return suppressed_; }
  [[nodiscard]] std::uint64_t markers() const noexcept { return markers_; }
  [[nodiscard]] std::uint64_t reported_lost() const noexcept { return reported_lost_; }
  [[nodiscard]] bool gap_aware() const noexcept { return gap_aware_; }

  [[nodiscard]] std::map<std::uint32_t, IdStats> per_id() const {
    std::map<std::uint32_t, IdStats> out;
    for (const auto& [id, s] : ids_) {
      out[id] = s.stats;
    }
    return out;
  }

 private:
  struct State {
    std::uint64_t last_ns = 0;
    bool have_last = false;
    bool resync = false;
    std::vector<std::uint64_t> samples;
    IdStats stats;
  };

  std::size_t calibrate_;
  double factor_;
  bool gap_aware_;

  std::map<std::uint32_t, State> ids_;
  std::uint64_t frames_ = 0;
  std::uint64_t anomalies_ = 0;
  std::uint64_t suppressed_ = 0;
  std::uint64_t markers_ = 0;
  std::uint64_t reported_lost_ = 0;
};

}  // namespace etg
