// Step-3 tap: opens one source, prints what comes out, reports the counters.
//
// This is the first visible sign of life and the throwaway shape of the real
// gateway. It stays single-threaded on purpose; threads arrive with the ring.

#include <poll.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "can_source.hpp"
#include "source.hpp"
#include "synthetic_source.hpp"
#include "version.hpp"

namespace {

void usage() {
  std::fprintf(stderr,
               "edge-telemetry-gateway %s\n"
               "\n"
               "usage: etg-gateway [--source can:<ifname>|synth] [--seconds N] [--quiet]\n"
               "\n"
               "  --source can:vcan0   read a SocketCAN interface\n"
               "  --source synth       generate frames in process (no vcan needed)\n"
               "  --seconds N          run for N seconds, then print stats (0 = forever)\n"
               "  --quiet              counters only, do not print each frame\n",
               etg::version().data());
}

void print_frame(const etg::Frame& f) {
  std::printf("src=%u id=%03X len=%u flags=%02X t_ingest=%llu t_kernel=%llu data=",
              static_cast<unsigned>(f.src_id), f.can_id, static_cast<unsigned>(f.len),
              static_cast<unsigned>(f.flags),
              static_cast<unsigned long long>(f.t_ingest_ns),
              static_cast<unsigned long long>(f.t_kernel_ns));
  for (std::uint8_t i = 0; i < f.len; ++i) {
    std::printf("%02X", f.data[i]);
  }
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string source_spec = "synth";
  int seconds = 5;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--source" && i + 1 < argc) {
      source_spec = argv[++i];
    } else if (arg == "--seconds" && i + 1 < argc) {
      seconds = std::atoi(argv[++i]);
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n\n", arg.c_str());
      usage();
      return 2;
    }
  }

  std::string error;
  std::unique_ptr<etg::Source> source;

  if (source_spec.rfind("can:", 0) == 0) {
    source = etg::CanSource::open(source_spec.substr(4), 0, /*receive_errors=*/true, error);
  } else if (source_spec == "synth") {
    source = etg::SyntheticSource::open(etg::SyntheticSource::Config{}, error);
  } else {
    std::fprintf(stderr, "unknown source: %s\n", source_spec.c_str());
    return 2;
  }

  if (!source) {
    std::fprintf(stderr, "failed to open source %s: %s\n", source_spec.c_str(), error.c_str());
    return 1;
  }

  std::fprintf(stderr, "reading %s (fd %d)\n", std::string{source->name()}.c_str(), source->fd());

  const std::uint64_t deadline =
      seconds > 0 ? etg::monotonic_ns() + static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL
                  : UINT64_MAX;

  std::array<etg::Frame, 64> batch{};

  while (etg::monotonic_ns() < deadline) {
    struct pollfd pfd {};
    pfd.fd = source->fd();
    pfd.events = POLLIN;

    const int rc = ::poll(&pfd, 1, 200);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::fprintf(stderr, "poll: %s\n", std::strerror(errno));
      return 1;
    }
    if (rc == 0) {
      continue;
    }

    const std::size_t n = source->drain(batch);
    if (!quiet) {
      for (std::size_t i = 0; i < n; ++i) {
        print_frame(batch[i]);
      }
    }
  }

  const etg::SourceStats& s = source->stats();
  std::fprintf(stderr,
               "\n%s: frames=%llu kernel_drops=%llu read_errors=%llu malformed=%llu\n",
               std::string{source->name()}.c_str(),
               static_cast<unsigned long long>(s.frames),
               static_cast<unsigned long long>(s.kernel_drops),
               static_cast<unsigned long long>(s.read_errors),
               static_cast<unsigned long long>(s.malformed));
  return 0;
}
