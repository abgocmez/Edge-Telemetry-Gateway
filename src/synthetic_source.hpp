#pragma once

#include <memory>
#include <string>
#include <vector>

#include "source.hpp"

namespace etg {

// An in-process frame generator, paced by a timerfd so it satisfies the Source
// contract without a socket.
//
// It exists for two reasons. Locally it is the only usable source at all: the
// default WSL2 kernel has CONFIG_CAN=m and CONFIG_CAN_RAW=m but not
// CONFIG_CAN_VCAN, so no virtual interface can be created. In measurement it is
// the source that establishes the pipeline's throughput ceiling, because it has
// no syscall per frame to hide behind.
//
// Known limitation, stated rather than hidden: pacing is per tick, so a tick
// that asks for more than one frame delivers them as a burst. A burst inflates
// the observed latency tail with a queueing delay that belongs to the generator
// and not to the gateway. Use one frame per tick when the number being measured
// is latency; use bursts only for throughput.
class SyntheticSource final : public Source {
 public:
  struct Config {
    std::uint8_t src_id = 0;
    std::uint32_t period_ns = 1'000'000;      // one tick per millisecond
    std::uint32_t frames_per_tick = 1;
    std::vector<std::uint32_t> can_ids{0x100, 0x200, 0x300};
  };

  static std::unique_ptr<SyntheticSource> open(const Config& cfg, std::string& error);

  ~SyntheticSource() override;

  [[nodiscard]] int fd() const noexcept override { return timer_fd_; }
  [[nodiscard]] std::uint8_t id() const noexcept override { return cfg_.src_id; }
  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] std::size_t drain(std::span<Frame> out) noexcept override;
  [[nodiscard]] const SourceStats& stats() const noexcept override { return stats_; }

 private:
  SyntheticSource(const Config& cfg, int timer_fd);

  Config cfg_;
  int timer_fd_;
  std::string name_;
  SourceStats stats_{};
  std::uint64_t counter_ = 0;   // drives the id cycle and the payload
  std::uint64_t pending_ = 0;   // frames owed from ticks already observed
};

}  // namespace etg
