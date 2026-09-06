// Consumer: connects to a gateway port, decodes the stream, and reports rate,
// loss and latency.
//
// Two latencies are measured because they are different things and conflating
// them would flatter the gateway:
//
//   gateway   now - frame.t_ingest_ns
//             ingest thread to this consumer. Everything the gateway is
//             responsible for: fan-out, queueing, encoding, TCP.
//
//   e2e       now - the CLOCK_MONOTONIC timestamp etg-gen wrote into the
//             payload. Adds the generator's own send path, the kernel, and the
//             vcan loopback. Only meaningful for frames etg-gen produced.
//
// Both subtract two CLOCK_MONOTONIC readings, so **both are only valid when the
// producer and this probe run on the same machine**. Across machines monotonic
// clocks share no epoch and the difference is meaningless - a large negative
// minimum is exactly what that mistake looks like, which is why the minimum is
// reported rather than clamped. Cross-machine measurement goes through the
// chrony path in M4.
//
// Gap detection is why seq is on the wire. A consumer that cannot tell "nothing
// was sent" from "something was sent and I lost it" cannot make any honest
// statement about the data it holds.

#include <csignal>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "frame_stream.hpp"
#include "gap_tracker.hpp"
#include "rt.hpp"
#include "samples.hpp"
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
               "  --reconnect      keep reconnecting if the gateway goes away\n"
               "  --warmup N       discard the first N seconds of latency samples;\n"
               "                   without it the tail is the age of the backlog that\n"
               "                   was waiting when this consumer connected\n"
               "  --e2e            also measure end-to-end latency; requires frames\n"
               "                   produced by etg-gen, which puts its send timestamp\n"
               "                   in the payload\n"
               "  --stall-us N     sleep N microseconds per batch, to make this\n"
               "                   consumer deliberately too slow to keep up\n"
               "  --cross          measure against CLOCK_REALTIME instead, for a\n"
               "                   gateway on another machine. Check both hosts are\n"
               "                   disciplined first - chronyc sources, and look for\n"
               "                   a non-zero Reach - or this reports the offset\n"
               "                   between two clocks rather than a latency\n"
               "  --csv PATH       write every retained gateway-latency sample\n"
               "  --mlock          lock the process into RAM, so a page fault\n"
               "                   cannot stall the instrument itself\n",
               etg::version().data());
}

std::uint64_t payload_timestamp(const etg::Frame& f) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(f.data[i]) << (8U * i);
  }
  return v;
}

void report(const char* label, const etg::Samples& s) {
  const etg::Percentiles p = s.compute();
  if (p.count == 0) {
    std::fprintf(stderr, "%-8s no samples\n", label);
    return;
  }
  std::fprintf(stderr,
               "%-8s n=%llu (retained %zu, stride %llu)\n"
               "         min %lld  p50 %lld  p90 %lld  p99 %lld  p99.9 %lld  max %lld  mean %.0f\n",
               label, static_cast<unsigned long long>(p.count), s.retained(),
               static_cast<unsigned long long>(s.stride()), static_cast<long long>(p.min),
               static_cast<long long>(p.p50), static_cast<long long>(p.p90),
               static_cast<long long>(p.p99), static_cast<long long>(p.p999),
               static_cast<long long>(p.max), p.mean);
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::string csv_path;
  bool mlock = false;
  std::uint16_t port = 9001;
  int seconds = 0;
  bool reconnect = false;
  bool measure_e2e = false;
  int warmup = 0;
  int stall_us = 0;
  bool cross_machine = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if (arg == "--host" && has_value) {
      host = argv[++i];
    } else if (arg == "--port" && has_value) {
      port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--seconds" && has_value) {
      seconds = std::atoi(argv[++i]);
    } else if (arg == "--warmup" && has_value) {
      warmup = std::atoi(argv[++i]);
    } else if (arg == "--stall-us" && has_value) {
      stall_us = std::atoi(argv[++i]);
    } else if (arg == "--mlock") {
      mlock = true;
    } else if (arg == "--csv" && has_value) {
      csv_path = argv[++i];
    } else if (arg == "--reconnect") {
      reconnect = true;
    } else if (arg == "--cross") {
      cross_machine = true;
    } else if (arg == "--e2e") {
      measure_e2e = true;
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (mlock) {
    std::string mlock_error;
    if (!etg::rt::lock_memory(mlock_error)) {
      // Asked for residency and did not get it. Carrying on would report
      // the same numbers as a run that had it, which is the one outcome
      // worth avoiding.
      std::fprintf(stderr, "%s\n", mlock_error.c_str());
      return 1;
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const std::uint64_t deadline =
      seconds > 0 ? etg::monotonic_ns() + static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL
                  : UINT64_MAX;

  std::uint64_t frames = 0;
  std::uint64_t batches = 0;
  std::uint64_t protocol_errs = 0;
  std::uint64_t reconnects = 0;
  etg::GapTracker gaps;

  etg::Samples gw_latency;

  // The once-a-second progress line needs a median, and the obvious way to get
  // one is to ask Samples for it. That was wrong, and wrong in the worst way a
  // measurement tool can be: Samples::compute copies and sorts everything it has
  // retained, this loop is the loop being measured, and the retained set grows
  // for the whole run. The stall it caused therefore grew with run length and
  // landed entirely in the tail it was reporting. At 20 000 frames/s a twelve
  // second run reported max 21 ms and a forty-five second run 124 ms, from the
  // same pipeline under the same load - the difference was 240k against 900k
  // samples being sorted once a second inside the read loop.
  //
  // So the live figure comes from a bounded window that is cleared each
  // interval: fixed cost, no growth, and it reports the median of the last
  // second rather than of the whole run, which is what a progress line should
  // say anyway. Full percentiles are computed once, after the loop has stopped.
  constexpr std::size_t kLiveWindow = 4096;
  std::vector<std::int64_t> live;
  live.reserve(kLiveWindow);
  etg::Samples e2e_latency;

  std::vector<etg::Frame> batch;
  std::uint64_t next_report = etg::monotonic_ns() + 1'000'000'000ULL;
  std::uint64_t last_frames = 0;
  const std::uint64_t start = etg::monotonic_ns();

  // Latency samples taken before this instant are discarded.
  //
  // Not cosmetic. Anything a consumer connects to has a backlog waiting for it,
  // and those frames were queued long before the consumer existed, so their
  // measured latency is the age of the backlog rather than the behaviour of the
  // pipeline. It shows up as a tail equal to the queue depth divided by the
  // rate, which is a property of the buffer, not of the system under test.
  // Reporting that as p99 would be dishonest in the flattering direction for
  // throughput claims and the damning direction for latency claims.
  const std::uint64_t measure_from =
      start + static_cast<std::uint64_t>(warmup > 0 ? warmup : 0) * 1'000'000'000ULL;
  std::uint64_t warmup_discarded = 0;

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
    ++reconnects;
    // Not loss: this consumer was simply not there for whatever it missed.
    gaps.reset();
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

      // The overload lever. Nothing about the pipeline changes; this consumer
      // simply cannot keep up, which is the only honest way to test what happens
      // to one that cannot.
      if (stall_us > 0 && st == etg::FrameStream::Status::kOk) {
        std::this_thread::sleep_for(std::chrono::microseconds{stall_us});
      }

      if (st == etg::FrameStream::Status::kOk) {
        // One reading for the whole batch, which is not an approximation: these
        // frames genuinely did arrive in the same read.
        // Which clock depends on where the gateway is.
        //
        // t_ingest_ns is CLOCK_MONOTONIC, the right choice on one machine: no
        // steps, no discipline, no NTP. Across machines it is meaningless -
        // monotonic clocks share no epoch, so subtracting them yields the
        // difference between two boot times. Measured across this pair it gave
        // a minimum of -741 seconds, which is why the minimum is reported
        // rather than clamped. t_kernel_ns is CLOCK_REALTIME, which chrony
        // disciplines on both hosts, and is the only field comparable at all.
        const std::uint64_t now =
            cross_machine ? etg::realtime_ns() : etg::monotonic_ns();
        ++batches;

        for (const etg::Frame& f : batch) {
          // Returned immediately and before anything else. The gateway times the
          // round trip with its own clock, so every microsecond spent here lands
          // in its figure - which is why the reply goes out before the frame is
          // counted, not after.
          if (etg::wire::is_echo(f)) {
            static_cast<void>(stream->send_back(f));
            continue;
          }
          // A marker is not a frame: it never existed on a bus, so measuring
          // latency against it would inject a fabricated sample.
          if (!gaps.observe(f)) {
            continue;
          }
          ++frames;

          if (now < measure_from) {
            ++warmup_discarded;
            continue;
          }

          const std::uint64_t stamp = cross_machine ? f.t_kernel_ns : f.t_ingest_ns;
          const std::int64_t delay =
              static_cast<std::int64_t>(now) - static_cast<std::int64_t>(stamp);
          gw_latency.add(delay);
          if (live.size() < kLiveWindow) {
            live.push_back(delay);
          }
          if (measure_e2e) {
            e2e_latency.add(static_cast<std::int64_t>(now) -
                            static_cast<std::int64_t>(payload_timestamp(f)));
          }
        }
      }

      const std::uint64_t now = etg::monotonic_ns();
      if (now >= next_report) {
        next_report = now + 1'000'000'000ULL;
        std::int64_t live_p50 = 0;
        if (!live.empty()) {
          // nth_element, not sort: the line needs one order statistic, and
          // this is linear where a sort is not.
          const std::size_t mid = live.size() / 2;
          std::nth_element(live.begin(), live.begin() + static_cast<std::ptrdiff_t>(mid),
                           live.end());
          live_p50 = live[mid];
        }
        std::fprintf(stderr, "%llu/s total=%llu lost=%llu gw_p50=%lldns\n",
                     static_cast<unsigned long long>(frames - last_frames),
                     static_cast<unsigned long long>(frames),
                     static_cast<unsigned long long>(gaps.total_lost()),
                     static_cast<long long>(live_p50));
        live.clear();
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
               "markers       %llu  (%llu frames, gateway-reported)\n"
               "absent        %llu  (%llu frames passed while disconnected)\n"
               "silent jumps  %llu  (%llu frames, no marker: a protocol fault)\n"
               "connections   %llu\n"
               "protocol_errs %llu\n"
               "warmup        %llu samples discarded (first %ds)\n"
               "\n"
               "latency, nanoseconds (%s):\n",
               static_cast<unsigned long long>(frames), elapsed,
               elapsed > 0.0 ? static_cast<double>(frames) / elapsed : 0.0,
               static_cast<unsigned long long>(batches),
               batches > 0 ? static_cast<double>(frames) / static_cast<double>(batches) : 0.0,
               static_cast<unsigned long long>(gaps.stats().markers),
               static_cast<unsigned long long>(gaps.stats().reported_lost),
               static_cast<unsigned long long>(gaps.stats().absent_markers),
               static_cast<unsigned long long>(gaps.stats().absent_frames),
               static_cast<unsigned long long>(gaps.stats().silent_jumps),
               static_cast<unsigned long long>(gaps.stats().silent_lost),
               static_cast<unsigned long long>(reconnects),
               static_cast<unsigned long long>(protocol_errs),
               static_cast<unsigned long long>(warmup_discarded), warmup,
               cross_machine
                   ? "CLOCK_REALTIME across hosts: accurate only to the clock discipline"
                   : "CLOCK_MONOTONIC, valid only within one machine");

  report("gateway", gw_latency);
  if (measure_e2e) {
    report("e2e", e2e_latency);
  }

  if (!csv_path.empty()) {
    if (gw_latency.write_csv(csv_path, "gateway_latency_ns")) {
      std::fprintf(stderr, "\nwrote %s\n", csv_path.c_str());
    } else {
      std::fprintf(stderr, "\nfailed to write %s: %s\n", csv_path.c_str(), std::strerror(errno));
    }
  }

  // Loss is reported, never hidden, but it is not a probe failure: under
  // at-most-once delivery a gap is the system working as designed.
  // A silent jump means the gateway lost frames and did not say so: a fault in
  // the mechanism, not in the data. Loss itself is not a failure - under
  // at-most-once, a reported gap is the system working as designed.
  return (protocol_errs > 0 || gaps.stats().silent_jumps > 0) ? 1 : 0;
}
