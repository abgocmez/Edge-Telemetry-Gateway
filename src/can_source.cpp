#include "can_source.hpp"

#include <linux/can.h>
#include <linux/net_tstamp.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace etg {
namespace {

// The kernel ABI for SCM_TIMESTAMPING, declared here rather than pulled from
// <linux/errqueue.h>, which collides with the glibc networking headers.
struct ScmTimestamping {
  struct timespec ts[3];
};

constexpr std::size_t kControlSize = 256;

std::uint64_t to_ns(const struct timespec& ts) noexcept {
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint8_t translate_flags(canid_t id) noexcept {
  std::uint8_t f = 0;
  if ((id & CAN_EFF_FLAG) != 0U) f = static_cast<std::uint8_t>(f | frame_flags::kExtendedId);
  if ((id & CAN_RTR_FLAG) != 0U) f = static_cast<std::uint8_t>(f | frame_flags::kRemote);
  if ((id & CAN_ERR_FLAG) != 0U) f = static_cast<std::uint8_t>(f | frame_flags::kError);
  return f;
}

}  // namespace

std::unique_ptr<CanSource> CanSource::open(const std::string& ifname, std::uint8_t src_id,
                                           bool receive_errors, std::string& error) {
  if (ifname.size() >= IFNAMSIZ) {
    error = "interface name too long: " + ifname;
    return nullptr;
  }

  const int fd = ::socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
  if (fd < 0) {
    error = "socket(PF_CAN): " + std::string{std::strerror(errno)};
    return nullptr;
  }

  auto fail = [&](const std::string& what) -> std::unique_ptr<CanSource> {
    error = what + ": " + std::strerror(errno);
    ::close(fd);
    return nullptr;
  };

  struct ifreq ifr {};
  std::memcpy(ifr.ifr_name, ifname.c_str(), ifname.size() + 1);
  if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
    return fail("ioctl(SIOCGIFINDEX) on " + ifname);
  }

  struct sockaddr_can addr {};
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    return fail("bind " + ifname);
  }

  // Kernel receive timestamps, in CLOCK_REALTIME. See docs/wire-format.md for
  // why this is a different clock domain from t_ingest_ns and must never be
  // subtracted from it directly.
  const int ts_flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
  if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &ts_flags, sizeof(ts_flags)) < 0) {
    return fail("setsockopt(SO_TIMESTAMPING)");
  }

  // Count what the kernel dropped before we saw it.
  const int on = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on)) < 0) {
    return fail("setsockopt(SO_RXQ_OVFL)");
  }

  // vcan is a loopback, so this is set deliberately rather than inherited: we
  // are a tap and must not receive frames this process itself sent.
  const int recv_own = 0;
  if (::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &recv_own, sizeof(recv_own)) < 0) {
    return fail("setsockopt(CAN_RAW_RECV_OWN_MSGS)");
  }

  if (receive_errors) {
    const can_err_mask_t err_mask = CAN_ERR_MASK;
    if (::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask)) < 0) {
      return fail("setsockopt(CAN_RAW_ERR_FILTER)");
    }
  }

  return std::unique_ptr<CanSource>{new CanSource(fd, src_id, ifname)};
}

CanSource::CanSource(int fd, std::uint8_t src_id, std::string ifname)
    : fd_(fd), src_id_(src_id), ifname_(std::move(ifname)) {}

CanSource::~CanSource() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

std::size_t CanSource::drain(std::span<Frame> out) noexcept {
  const std::size_t want = out.size() < kBatch ? out.size() : kBatch;
  if (want == 0) {
    return 0;
  }

  std::array<struct mmsghdr, kBatch> msgs{};
  std::array<struct iovec, kBatch> iovs{};
  std::array<struct can_frame, kBatch> raw{};
  std::array<std::array<char, kControlSize>, kBatch> control{};

  for (std::size_t i = 0; i < want; ++i) {
    iovs[i].iov_base = &raw[i];
    iovs[i].iov_len = sizeof(raw[i]);
    msgs[i].msg_hdr.msg_iov = &iovs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
    msgs[i].msg_hdr.msg_control = control[i].data();
    // Reset on every call: the kernel writes the used length back into this
    // field, so a stale value silently truncates the next batch's timestamps.
    msgs[i].msg_hdr.msg_controllen = kControlSize;
  }

  const int n = ::recvmmsg(fd_, msgs.data(), static_cast<unsigned>(want), MSG_DONTWAIT, nullptr);
  if (n <= 0) {
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      ++stats_.read_errors;
    }
    return 0;
  }

  const std::uint64_t t_ingest = monotonic_ns();
  std::size_t produced = 0;

  for (int i = 0; i < n; ++i) {
    if (msgs[i].msg_len < sizeof(struct can_frame)) {
      ++stats_.malformed;
      continue;
    }

    std::uint64_t t_kernel = 0;
    // CMSG_NXTHDR takes a non-const cmsghdr*, so this cannot be const.
    for (cmsghdr* cm = CMSG_FIRSTHDR(&msgs[i].msg_hdr); cm != nullptr;
         cm = CMSG_NXTHDR(&msgs[i].msg_hdr, cm)) {
      if (cm->cmsg_level != SOL_SOCKET) {
        continue;
      }
      if (cm->cmsg_type == SO_TIMESTAMPING) {
        ScmTimestamping ts{};
        std::memcpy(&ts, CMSG_DATA(cm), sizeof(ts));
        t_kernel = to_ns(ts.ts[0]);  // ts[0] is the software receive timestamp
      } else if (cm->cmsg_type == SO_RXQ_OVFL) {
        std::uint32_t ovfl = 0;
        std::memcpy(&ovfl, CMSG_DATA(cm), sizeof(ovfl));
        if (have_ovfl_) {
          stats_.kernel_drops += static_cast<std::uint64_t>(ovfl - last_ovfl_);
        }
        last_ovfl_ = ovfl;
        have_ovfl_ = true;
      }
    }

    const struct can_frame& cf = raw[i];
    Frame& f = out[produced];
    f.seq = 0;  // assigned at ring-claim time; a source cannot know global order
    f.t_kernel_ns = t_kernel;
    f.t_ingest_ns = t_ingest;
    f.flags = translate_flags(cf.can_id);
    const canid_t mask =
        (f.flags & frame_flags::kExtendedId) != 0U ? CAN_EFF_MASK : CAN_SFF_MASK;
    f.can_id = cf.can_id & mask;
    f.src_id = src_id_;
    f.len = cf.can_dlc > kMaxPayload ? static_cast<std::uint8_t>(kMaxPayload) : cf.can_dlc;
    f.reserved = 0;
    f.data = {};
    std::memcpy(f.data.data(), cf.data, f.len);
    ++produced;
  }

  stats_.frames += produced;
  return produced;
}

}  // namespace etg
