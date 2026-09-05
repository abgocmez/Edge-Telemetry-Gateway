// Paced CAN frame generator.
//
// This exists instead of cangen because cangen paces with sleeps and reports
// nothing about whether it achieved the rate it was asked for. A load generator
// that cannot prove its own output was flat makes every latency number measured
// downstream of it unfalsifiable.
//
// Two things make the pacing here defensible:
//
//   * The schedule is absolute. Tick k is due at start + (k+1)*period, not at
//     "one period after the last tick". Measuring against the previous tick
//     hides cumulative drift, because a run that is uniformly 20% slow shows
//     near-zero inter-tick error while landing nowhere near the requested rate.
//
//   * timerfd reports missed expirations, so a tick lost to scheduling is owed
//     and paid rather than silently skipped.
//
// The payload carries the CLOCK_MONOTONIC send timestamp, which is what makes
// end-to-end latency measurable on the local path without any clock-domain
// conversion. Frames the kernel refuses (ENOBUFS on a full qdisc) are counted
// and never retried: a retry would distort the very pacing being measured.

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "samples.hpp"
#include "source.hpp"
#include "version.hpp"

namespace {

void usage() {
  std::fprintf(stderr,
               "etg-gen %s - paced CAN frame generator\n"
               "\n"
               "usage: etg-gen --interface vcan0 [options]\n"
               "\n"
               "  --interface NAME   CAN interface to write to (required)\n"
               "  --rate N           frames per second (default 1000)\n"
               "  --seconds N        run duration (default 10)\n"
               "  --id HEX           base CAN id (default 100)\n"
               "  --ids N            cycle through N consecutive ids (default 1)\n"
               "  --csv PATH         write every retained jitter sample, one per line\n",
               etg::version().data());
}

int open_can_writer(const std::string& ifname, std::string& error) {
  if (ifname.size() >= IFNAMSIZ) {
    error = "interface name too long: " + ifname;
    return -1;
  }

  const int fd = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
  if (fd < 0) {
    error = "socket(PF_CAN): " + std::string{std::strerror(errno)};
    return -1;
  }

  struct ifreq ifr {};
  std::memcpy(ifr.ifr_name, ifname.c_str(), ifname.size() + 1);
  if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
    error = "ioctl(SIOCGIFINDEX) on " + ifname + ": " + std::strerror(errno);
    ::close(fd);
    return -1;
  }

  struct sockaddr_can addr {};
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    error = "bind " + ifname + ": " + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

int main(int argc, char** argv) {
  std::string ifname;
  std::string csv_path;
  std::uint64_t rate = 1000;
  std::uint64_t seconds = 10;
  std::uint32_t base_id = 0x100;
  std::uint32_t id_count = 1;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if (arg == "--interface" && has_value) {
      ifname = argv[++i];
    } else if (arg == "--rate" && has_value) {
      rate = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--seconds" && has_value) {
      seconds = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--id" && has_value) {
      base_id = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 16));
    } else if (arg == "--ids" && has_value) {
      id_count = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--csv" && has_value) {
      csv_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (ifname.empty() || rate == 0 || seconds == 0 || id_count == 0) {
    usage();
    return 2;
  }

  const std::uint64_t period_ns = 1'000'000'000ULL / rate;
  if (period_ns == 0) {
    std::fprintf(stderr, "rate %llu is beyond nanosecond resolution\n",
                 static_cast<unsigned long long>(rate));
    return 2;
  }
  if (period_ns < 50'000) {
    std::fprintf(stderr,
                 "warning: requested period %lluns is near the practical timerfd floor; "
                 "expect the achieved rate to fall short of the request\n",
                 static_cast<unsigned long long>(period_ns));
  }

  std::string error;
  const int can_fd = open_can_writer(ifname, error);
  if (can_fd < 0) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  const int timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  if (timer_fd < 0) {
    std::fprintf(stderr, "timerfd_create: %s\n", std::strerror(errno));
    ::close(can_fd);
    return 1;
  }

  struct itimerspec spec {};
  spec.it_interval.tv_sec = static_cast<time_t>(period_ns / 1'000'000'000ULL);
  spec.it_interval.tv_nsec = static_cast<long>(period_ns % 1'000'000'000ULL);
  spec.it_value = spec.it_interval;

  const std::uint64_t total = rate * seconds;
  etg::Samples jitter;

  const std::uint64_t start = etg::monotonic_ns();
  if (::timerfd_settime(timer_fd, 0, &spec, nullptr) < 0) {
    std::fprintf(stderr, "timerfd_settime: %s\n", std::strerror(errno));
    ::close(timer_fd);
    ::close(can_fd);
    return 1;
  }

  std::uint64_t sent = 0;
  std::uint64_t enobufs = 0;
  std::uint64_t send_errors = 0;
  std::uint64_t k = 0;

  std::fprintf(stderr, "etg-gen: %s rate=%llu/s period=%lluns frames=%llu\n", ifname.c_str(),
               static_cast<unsigned long long>(rate),
               static_cast<unsigned long long>(period_ns),
               static_cast<unsigned long long>(total));

  while (k < total) {
    std::uint64_t expirations = 0;
    const ssize_t n = ::read(timer_fd, &expirations, sizeof(expirations));
    if (n != static_cast<ssize_t>(sizeof(expirations))) {
      if (errno == EINTR) {
        continue;
      }
      std::fprintf(stderr, "timerfd read: %s\n", std::strerror(errno));
      break;
    }

    for (std::uint64_t e = 0; e < expirations && k < total; ++e, ++k) {
      const std::uint64_t ideal = start + (k + 1) * period_ns;
      const std::uint64_t now = etg::monotonic_ns();
      jitter.add(static_cast<std::int64_t>(now) - static_cast<std::int64_t>(ideal));

      struct can_frame cf {};
      cf.can_id = base_id + static_cast<std::uint32_t>(k % id_count);
      cf.can_dlc = 8;
      // CLOCK_MONOTONIC send timestamp, little-endian: the consumer subtracts
      // this from its own monotonic clock to get end-to-end latency on the
      // local path with no clock-domain conversion at all.
      for (std::size_t b = 0; b < 8; ++b) {
        cf.data[b] = static_cast<std::uint8_t>((now >> (8U * b)) & 0xFFU);
      }

      const ssize_t w = ::write(can_fd, &cf, sizeof(cf));
      if (w == static_cast<ssize_t>(sizeof(cf))) {
        ++sent;
      } else if (errno == ENOBUFS) {
        ++enobufs;  // qdisc full: a generator-side loss, reported, never retried
      } else {
        ++send_errors;
      }
    }
  }

  const std::uint64_t elapsed = etg::monotonic_ns() - start;
  const double elapsed_s = static_cast<double>(elapsed) / 1e9;
  const double achieved = elapsed_s > 0.0 ? static_cast<double>(sent) / elapsed_s : 0.0;
  const etg::Percentiles p = jitter.compute();

  std::fprintf(stderr,
               "\n"
               "requested   %llu frames at %llu/s\n"
               "sent        %llu in %.3fs = %.1f/s (%.2f%% of requested)\n"
               "enobufs     %llu\n"
               "send_errors %llu\n"
               "\n"
               "pacing error vs the absolute schedule, nanoseconds:\n"
               "  samples %llu (retained %zu, stride %llu)\n"
               "  min %lld  p50 %lld  p90 %lld  p99 %lld  p99.9 %lld  max %lld  mean %.0f\n",
               static_cast<unsigned long long>(total), static_cast<unsigned long long>(rate),
               static_cast<unsigned long long>(sent), elapsed_s, achieved,
               100.0 * achieved / static_cast<double>(rate),
               static_cast<unsigned long long>(enobufs),
               static_cast<unsigned long long>(send_errors),
               static_cast<unsigned long long>(p.count), jitter.retained(),
               static_cast<unsigned long long>(jitter.stride()),
               static_cast<long long>(p.min), static_cast<long long>(p.p50),
               static_cast<long long>(p.p90), static_cast<long long>(p.p99),
               static_cast<long long>(p.p999), static_cast<long long>(p.max), p.mean);

  if (!csv_path.empty()) {
    if (jitter.write_csv(csv_path, "pacing_error_ns")) {
      std::fprintf(stderr, "\nwrote %s\n", csv_path.c_str());
    } else {
      std::fprintf(stderr, "\nfailed to write %s: %s\n", csv_path.c_str(), std::strerror(errno));
    }
  }

  ::close(timer_fd);
  ::close(can_fd);
  return 0;
}
