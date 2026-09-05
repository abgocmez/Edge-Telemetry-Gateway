// Consumer: connects to a gateway port, decodes the stream, reports rate and
// loss.
//
// Step 6 scope is deliberately narrow - connect, decode, count, detect gaps.
// The latency histogram arrives in step 7 on top of the Samples type that
// already exists.
//
// Gap detection is the reason seq is on the wire at all. A consumer that cannot
// tell "nothing was sent" from "something was sent and I lost it" cannot make
// any honest statement about the data it holds.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "frame_stream.hpp"
#include "source.hpp"
#include "version.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

void usage() {
  std::fprintf(stderr,
               "etg-probe %s - gateway consumer\n"
               "\n"
               "usage: etg-probe [--host H] [--port P] [--seconds N]\n"
               "\n"
               "  --host HOST      gateway host (default 127.0.0.1)\n"
               "  --port PORT      gateway port (default 9001)\n"
               "  --seconds N      run for N seconds (0 = until interrupted)\n"
               "  --reconnect      keep reconnecting if the gateway goes away\n",
               etg::version().data());
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::uint16_t port = 9001;
  int seconds = 0;
  bool reconnect = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if (arg == "--host" && has_value) {
      host = argv[++i];
    } else if (arg == "--port" && has_value) {
      port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--seconds" && has_value) {
      seconds = std::atoi(argv[++i]);
    } else if (arg == "--reconnect") {
      reconnect = true;
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n\n", arg.c_str());
      usage();
      return 2;
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const std::uint64_t deadline =
      seconds > 0 ? etg::monotonic_ns() + static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL
                  : UINT64_MAX;

  std::uint64_t frames = 0;
  std::uint64_t batches = 0;
  std::uint64_t gaps = 0;         // how many times seq jumped
  std::uint64_t missing = 0;      // how many frames those jumps accounted for
  std::uint64_t protocol_errs = 0;
  bool have_last = false;
  std::uint64_t last_seq = 0;

  std::vector<etg::Frame> batch;
  std::uint64_t next_report = etg::monotonic_ns() + 1'000'000'000ULL;
  std::uint64_t last_frames = 0;
  const std::uint64_t start = etg::monotonic_ns();

  while (g_stop == 0 && etg::monotonic_ns() < deadline) {
    std::string error;
    auto stream = etg::FrameStream::connect(host, port, error);
    if (!stream) {
      if (!reconnect) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
      }
      std::fprintf(stderr, "connect failed (%s), retrying\n", error.c_str());
      continue;
    }
    std::fprintf(stderr, "connected to %s:%u\n", host.c_str(), port);

    while (g_stop == 0 && etg::monotonic_ns() < deadline) {
      const etg::FrameStream::Status st = stream->read_batch(batch, 200);

      if (st == etg::FrameStream::Status::kClosed) {
        std::fprintf(stderr, "gateway closed the connection\n");
        break;
      }
      if (st == etg::FrameStream::Status::kProtocolError) {
        ++protocol_errs;
        std::fprintf(stderr, "protocol error: %s\n", etg::wire::to_string(stream->last_error()));
        break;
      }
      if (st == etg::FrameStream::Status::kTimeout) {
        // Nothing complete yet. Partial state is retained by the stream, so
        // this is not a resynchronisation point.
      } else {
        ++batches;
        for (const etg::Frame& f : batch) {
          if (have_last && f.seq != last_seq + 1) {
            ++gaps;
            missing += f.seq > last_seq ? (f.seq - last_seq - 1) : 0;
          }
          last_seq = f.seq;
          have_last = true;
          ++frames;
        }
      }

      const std::uint64_t now = etg::monotonic_ns();
      if (now >= next_report) {
        next_report = now + 1'000'000'000ULL;
        std::fprintf(stderr, "%llu/s total=%llu gaps=%llu missing=%llu\n",
                     static_cast<unsigned long long>(frames - last_frames),
                     static_cast<unsigned long long>(frames),
                     static_cast<unsigned long long>(gaps),
                     static_cast<unsigned long long>(missing));
        last_frames = frames;
      }
    }

    if (!reconnect) {
      break;
    }
  }

  const double elapsed = static_cast<double>(etg::monotonic_ns() - start) / 1e9;
  std::fprintf(stderr,
               "\n"
               "frames        %llu in %.3fs = %.1f/s\n"
               "batches       %llu (mean %.1f frames/batch)\n"
               "gaps          %llu\n"
               "missing       %llu frames\n"
               "protocol_errs %llu\n",
               static_cast<unsigned long long>(frames), elapsed,
               elapsed > 0.0 ? static_cast<double>(frames) / elapsed : 0.0,
               static_cast<unsigned long long>(batches),
               batches > 0 ? static_cast<double>(frames) / static_cast<double>(batches) : 0.0,
               static_cast<unsigned long long>(gaps),
               static_cast<unsigned long long>(missing),
               static_cast<unsigned long long>(protocol_errs));

  // Loss is reported, never hidden, but it is not a failure of the probe: at
  // most-once delivery a gap is the system working as designed.
  return protocol_errs > 0 ? 1 : 0;
}
