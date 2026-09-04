#pragma once

#include <memory>
#include <string>

#include "source.hpp"

namespace etg {

// SocketCAN ingest: PF_CAN / SOCK_RAW / CAN_RAW. The code path is identical for
// a real controller and for a vcan interface, so only the name differs.
//
// Reads with recvmmsg, because at raw passthrough there is no per-frame CPU
// work to speak of and the syscall is the bottleneck.
//
// Two socket options carry the honesty of the measurements:
//   SO_TIMESTAMPING - the kernel receive timestamp, in CLOCK_REALTIME
//   SO_RXQ_OVFL     - frames the kernel dropped before we ever saw them
// Without the second, an overload test reports our own drop rate while the
// kernel silently discards frames upstream of us, and the published number is
// wrong in a way that nothing would reveal.
class CanSource final : public Source {
 public:
  static constexpr std::size_t kBatch = 64;

  // receive_errors enables CAN_RAW_ERR_FILTER. On vcan there is no controller
  // to produce error frames, but a frame crafted with CAN_ERR_FLAG can be sent
  // and will be received, which is enough to exercise the error path.
  static std::unique_ptr<CanSource> open(const std::string& ifname, std::uint8_t src_id,
                                         bool receive_errors, std::string& error);

  ~CanSource() override;

  [[nodiscard]] int fd() const noexcept override { return fd_; }
  [[nodiscard]] std::uint8_t id() const noexcept override { return src_id_; }
  [[nodiscard]] std::string_view name() const noexcept override { return ifname_; }
  [[nodiscard]] std::size_t drain(std::span<Frame> out) noexcept override;
  [[nodiscard]] const SourceStats& stats() const noexcept override { return stats_; }

 private:
  CanSource(int fd, std::uint8_t src_id, std::string ifname);

  int fd_;
  std::uint8_t src_id_;
  std::string ifname_;
  SourceStats stats_{};

  // SO_RXQ_OVFL reports a cumulative per-socket counter, so drops are the
  // delta. have_ovfl_ distinguishes "no drops yet" from "first sample".
  std::uint32_t last_ovfl_ = 0;
  bool have_ovfl_ = false;
};

}  // namespace etg
