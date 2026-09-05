// Recorder: connects to a gateway port and appends what it receives to a file.
//
// Deliberately dumb - open, append, count. Still no rotation and no retention
// policy; those remain out of scope.
//
// Gap markers are written to the file exactly as received. A capture that
// silently omitted its own holes would be a recording that lies about what it
// is missing, which is precisely the failure the marker mechanism exists to
// prevent - and it would make the file useless for any consumer that has to
// distinguish "the bus was quiet" from "I was not fast enough".
//
// The file is written as a stream of complete wire messages, header and all,
// rather than as bare records. It costs nothing, it makes the file
// self-describing under docs/wire-format.md, and it means a capture can be
// replayed by piping it straight into a consumer with no separate reader.
//
// This is also the naturally slow consumer. On the Pi it writes to an SD card,
// whose stalls are real and do not need simulating, so the write-duration
// distribution reported here is the evidence for that claim rather than an
// assertion about it.

#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "frame_stream.hpp"
#include "gap_tracker.hpp"
#include "samples.hpp"
#include "source.hpp"
#include "version.hpp"
#include "wire.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

void usage() {
  std::fprintf(stderr,
               "etg-record %s - gateway consumer that appends to a file\n"
               "\n"
               "usage: etg-record --out PATH [--host H] [--port P] [--seconds N]\n"
               "\n"
               "  --out PATH       output file (required)\n"
               "  --host HOST      gateway host (default 127.0.0.1)\n"
               "  --port PORT      gateway port (default 9002)\n"
               "  --seconds N      run for N seconds (0 = until interrupted)\n"
               "  --reconnect      keep reconnecting if the gateway goes away\n"
               "  --fsync-every N  fsync after every N batches (0 = never)\n"
               "  --csv PATH       write every retained write-duration sample\n",
               etg::version().data());
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::string out_path;
  std::string csv_path;
  std::uint16_t port = 9002;
  int seconds = 0;
  bool reconnect = false;
  std::uint64_t fsync_every = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if (arg == "--out" && has_value) {
      out_path = argv[++i];
    } else if (arg == "--host" && has_value) {
      host = argv[++i];
    } else if (arg == "--port" && has_value) {
      port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--seconds" && has_value) {
      seconds = std::atoi(argv[++i]);
    } else if (arg == "--fsync-every" && has_value) {
      fsync_every = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--csv" && has_value) {
      csv_path = argv[++i];
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

  if (out_path.empty()) {
    usage();
    return 2;
  }

  std::FILE* out = std::fopen(out_path.c_str(), "wb");
  if (out == nullptr) {
    std::fprintf(stderr, "cannot open %s: %s\n", out_path.c_str(), std::strerror(errno));
    return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const std::uint64_t deadline =
      seconds > 0 ? etg::monotonic_ns() + static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL
                  : UINT64_MAX;

  std::uint64_t frames = 0;
  std::uint64_t batches = 0;
  std::uint64_t bytes = 0;
  etg::GapTracker gaps;
  std::uint64_t write_errors = 0;
  std::uint64_t protocol_errs = 0;

  etg::Samples write_ns;
  std::vector<etg::Frame> batch;
  std::vector<std::byte> encoded;
  const std::uint64_t start = etg::monotonic_ns();
  std::uint64_t next_report = start + 1'000'000'000ULL;
  std::uint64_t last_frames = 0;

  while (g_stop == 0 && etg::monotonic_ns() < deadline) {
    std::string error;
    auto stream = etg::FrameStream::connect(host, port, error);
    if (!stream) {
      if (!reconnect) {
        std::fprintf(stderr, "%s\n", error.c_str());
        std::fclose(out);
        return 1;
      }
      std::fprintf(stderr, "connect failed (%s), retrying\n", error.c_str());
      continue;
    }
    gaps.reset();
    std::fprintf(stderr, "connected to %s:%u, writing %s\n", host.c_str(), port, out_path.c_str());

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
      if (st != etg::FrameStream::Status::kOk || batch.empty()) {
        continue;
      }

      // Markers are counted, and they are also written to the file exactly as
      // received. A capture that silently omitted its own holes would be a
      // recording that lies about what it is missing, which is the failure this
      // whole mechanism exists to prevent.
      for (const etg::Frame& f : batch) {
        if (etg::wire::is_echo(f)) {
          static_cast<void>(stream->send_back(f));
          continue;
        }
        static_cast<void>(gaps.observe(f));
      }

      // Re-encoded rather than written from the in-memory struct, for the same
      // reason the consumer parses instead of casting: the file is defined by
      // the wire spec, not by this compiler's layout.
      encoded.resize(etg::wire::kHeaderSize + batch.size() * etg::wire::kRecordSize);
      etg::wire::encode_header(static_cast<std::uint32_t>(batch.size()),
                               etg::wire::HeaderBytes{encoded.data(), etg::wire::kHeaderSize});
      std::byte* p = encoded.data() + etg::wire::kHeaderSize;
      for (const etg::Frame& f : batch) {
        etg::wire::encode_frame(f, etg::wire::FrameBytes{p, etg::wire::kRecordSize});
        p += etg::wire::kRecordSize;
      }

      const std::uint64_t t0 = etg::monotonic_ns();
      const std::size_t written = std::fwrite(encoded.data(), 1, encoded.size(), out);
      ++batches;
      if (fsync_every != 0 && batches % fsync_every == 0) {
        std::fflush(out);
        static_cast<void>(::fsync(::fileno(out)));
      }
      write_ns.add(static_cast<std::int64_t>(etg::monotonic_ns() - t0));

      if (written != encoded.size()) {
        ++write_errors;
      } else {
        bytes += encoded.size();
        frames += batch.size();
      }

      const std::uint64_t now = etg::monotonic_ns();
      if (now >= next_report) {
        next_report = now + 1'000'000'000ULL;
        std::fprintf(stderr, "%llu/s total=%llu bytes=%llu lost=%llu\n",
                     static_cast<unsigned long long>(frames - last_frames),
                     static_cast<unsigned long long>(frames),
                     static_cast<unsigned long long>(bytes),
                     static_cast<unsigned long long>(gaps.total_lost()));
        last_frames = frames;
      }
    }

    if (!reconnect) {
      break;
    }
  }

  std::fflush(out);
  std::fclose(out);

  const double elapsed = static_cast<double>(etg::monotonic_ns() - start) / 1e9;
  const etg::Percentiles p = write_ns.compute();
  std::fprintf(stderr,
               "\n"
               "frames        %llu in %.3fs = %.1f/s\n"
               "bytes         %llu\n"
               "batches       %llu\n"
               "markers       %llu  (%llu frames, gateway-reported)\n"
               "silent jumps  %llu  (%llu frames, no marker: a protocol fault)\n"
               "write_errors  %llu\n"
               "protocol_errs %llu\n"
               "\n"
               "write duration, nanoseconds:\n"
               "  n=%llu  min %lld  p50 %lld  p90 %lld  p99 %lld  p99.9 %lld  max %lld\n",
               static_cast<unsigned long long>(frames), elapsed,
               elapsed > 0.0 ? static_cast<double>(frames) / elapsed : 0.0,
               static_cast<unsigned long long>(bytes),
               static_cast<unsigned long long>(batches),
               static_cast<unsigned long long>(gaps.stats().markers),
               static_cast<unsigned long long>(gaps.stats().reported_lost),
               static_cast<unsigned long long>(gaps.stats().silent_jumps),
               static_cast<unsigned long long>(gaps.stats().silent_lost),
               static_cast<unsigned long long>(write_errors),
               static_cast<unsigned long long>(protocol_errs),
               static_cast<unsigned long long>(p.count), static_cast<long long>(p.min),
               static_cast<long long>(p.p50), static_cast<long long>(p.p90),
               static_cast<long long>(p.p99), static_cast<long long>(p.p999),
               static_cast<long long>(p.max));

  if (!csv_path.empty() && !write_ns.write_csv(csv_path, "write_duration_ns")) {
    std::fprintf(stderr, "failed to write %s\n", csv_path.c_str());
  }

  return (protocol_errs > 0 || write_errors > 0) ? 1 : 0;
}
