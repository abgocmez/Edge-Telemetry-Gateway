// The gateway: one source, one ingest thread, one queue and one egress thread
// per consumer.
//
//   source --> [fan-out: assign seq, copy into N queues] --> N egress threads --> TCP
//
// Still topology A. Every consumer costs one copy, and the queues are mutex
// guarded. The lock-free broadcast ring replaces the middle of this picture in
// M2, and this version stays as the baseline it is measured against.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "can_source.hpp"
#include "egress.hpp"
#include "fanout.hpp"
#include "source.hpp"
#include "synthetic_source.hpp"
#include "version.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

void usage() {
  std::fprintf(stderr,
               "etg-gateway %s\n"
               "\n"
               "usage: etg-gateway [--source can:<if>|synth] [--consumer NAME:PORT ...]\n"
               "\n"
               "  --source can:vcan0     read a SocketCAN interface\n"
               "  --source synth         generate frames in process (no vcan needed)\n"
               "  --consumer NAME:PORT   serve a consumer on PORT; repeatable\n"
               "  --queue N              per-consumer queue capacity in frames (default 8192)\n"
               "  --rate N               synthetic source rate in frames/s (default 1000)\n"
               "  --seconds N            run for N seconds (0 = until interrupted)\n"
               "  --tap                  print every frame; only useful at low rates\n",
               etg::version().data());
}

struct ConsumerSpec {
  std::string name;
  std::uint16_t port;
};

bool parse_consumer(const std::string& spec, ConsumerSpec& out) {
  const std::size_t colon = spec.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= spec.size()) {
    return false;
  }
  const long port = std::strtol(spec.substr(colon + 1).c_str(), nullptr, 10);
  if (port <= 0 || port > 65535) {
    return false;
  }
  out.name = spec.substr(0, colon);
  out.port = static_cast<std::uint16_t>(port);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string source_spec = "synth";
  std::vector<ConsumerSpec> consumers;
  std::size_t queue_capacity = 8192;
  std::uint32_t rate = 1000;
  int seconds = 0;
  bool tap = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if (arg == "--source" && has_value) {
      source_spec = argv[++i];
    } else if (arg == "--consumer" && has_value) {
      ConsumerSpec c;
      if (!parse_consumer(argv[++i], c)) {
        std::fprintf(stderr, "bad consumer spec: %s (want NAME:PORT)\n", argv[i]);
        return 2;
      }
      consumers.push_back(c);
    } else if (arg == "--queue" && has_value) {
      queue_capacity = std::strtoul(argv[++i], nullptr, 10);
    } else if (arg == "--rate" && has_value) {
      rate = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--seconds" && has_value) {
      seconds = std::atoi(argv[++i]);
    } else if (arg == "--tap") {
      tap = true;
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (queue_capacity == 0 || rate == 0) {
    usage();
    return 2;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::string error;
  std::unique_ptr<etg::Source> source;
  if (source_spec.rfind("can:", 0) == 0) {
    source = etg::CanSource::open(source_spec.substr(4), 0, /*receive_errors=*/true, error);
  } else if (source_spec == "synth") {
    etg::SyntheticSource::Config cfg;
    cfg.period_ns = 1'000'000'000U / rate;
    if (cfg.period_ns == 0) {
      cfg.period_ns = 1;
    }
    source = etg::SyntheticSource::open(cfg, error);
  } else {
    std::fprintf(stderr, "unknown source: %s\n", source_spec.c_str());
    return 2;
  }
  if (!source) {
    std::fprintf(stderr, "failed to open source %s: %s\n", source_spec.c_str(), error.c_str());
    return 1;
  }

  // One queue per consumer, plus one for --tap so printing cannot slow the
  // ingest thread down: the tap is just another consumer that may fall behind.
  std::vector<std::unique_ptr<etg::FrameQueue>> queues;
  std::vector<std::unique_ptr<etg::Egress>> egresses;
  std::vector<etg::FrameQueue*> sinks;

  for (const ConsumerSpec& c : consumers) {
    auto q = std::make_unique<etg::FrameQueue>(queue_capacity);
    auto e = etg::Egress::create(c.name, *q, c.port, error);
    if (!e) {
      std::fprintf(stderr, "consumer %s: %s\n", c.name.c_str(), error.c_str());
      return 1;
    }
    sinks.push_back(q.get());
    queues.push_back(std::move(q));
    egresses.push_back(std::move(e));
  }

  std::unique_ptr<etg::FrameQueue> tap_queue;
  if (tap) {
    tap_queue = std::make_unique<etg::FrameQueue>(queue_capacity);
    sinks.push_back(tap_queue.get());
  }

  if (sinks.empty()) {
    std::fprintf(stderr, "no consumers and no --tap: nothing to do\n\n");
    usage();
    return 2;
  }

  auto fan = etg::FanOut::create(*source, sinks, error);
  if (!fan) {
    std::fprintf(stderr, "fan-out: %s\n", error.c_str());
    return 1;
  }

  for (auto& e : egresses) {
    e->start();
    std::fprintf(stderr, "consumer %-10s listening on port %u\n", e->name().c_str(), e->port());
  }
  fan->start();
  std::fprintf(stderr, "reading %s\n", std::string{source->name()}.c_str());

  const std::uint64_t deadline =
      seconds > 0 ? etg::monotonic_ns() + static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL
                  : UINT64_MAX;

  std::vector<etg::Frame> tap_batch(256);
  std::uint64_t next_report = etg::monotonic_ns() + 1'000'000'000ULL;
  std::uint64_t last_frames = 0;

  while (g_stop == 0 && etg::monotonic_ns() < deadline) {
    if (tap_queue) {
      const std::size_t n =
          tap_queue->pop_batch(std::span<etg::Frame>{tap_batch}, std::chrono::milliseconds{100});
      for (std::size_t i = 0; i < n; ++i) {
        const etg::Frame& f = tap_batch[i];
        std::printf("seq=%llu src=%u id=%03X len=%u\n",
                    static_cast<unsigned long long>(f.seq), static_cast<unsigned>(f.src_id),
                    f.can_id, static_cast<unsigned>(f.len));
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    const std::uint64_t now = etg::monotonic_ns();
    if (now >= next_report) {
      next_report = now + 1'000'000'000ULL;
      const etg::FanOut::Stats fs = fan->stats();
      std::fprintf(stderr, "ingest %llu/s total=%llu",
                   static_cast<unsigned long long>(fs.frames - last_frames),
                   static_cast<unsigned long long>(fs.frames));
      last_frames = fs.frames;
      for (std::size_t i = 0; i < egresses.size(); ++i) {
        const etg::Egress::Stats es = egresses[i]->stats();
        const etg::QueueStats qs = queues[i]->stats();
        std::fprintf(stderr, " | %s %s sent=%llu drop=%llu wblock=%llu q=%zu",
                     egresses[i]->name().c_str(), es.connected ? "up" : "--",
                     static_cast<unsigned long long>(es.frames_sent),
                     static_cast<unsigned long long>(qs.dropped),
                     static_cast<unsigned long long>(es.would_block), qs.size);
      }
      std::fprintf(stderr, "\n");
    }
  }

  std::fprintf(stderr, "\nshutting down\n");
  fan->stop();
  for (auto& e : egresses) {
    e->stop();
  }

  const etg::FanOut::Stats fs = fan->stats();
  const etg::SourceStats ss = source->stats();
  std::fprintf(stderr, "\nsource   %s frames=%llu kernel_drops=%llu read_errors=%llu\n",
               std::string{source->name()}.c_str(),
               static_cast<unsigned long long>(ss.frames),
               static_cast<unsigned long long>(ss.kernel_drops),
               static_cast<unsigned long long>(ss.read_errors));
  std::fprintf(stderr, "ingest   frames=%llu batches=%llu\n",
               static_cast<unsigned long long>(fs.frames),
               static_cast<unsigned long long>(fs.batches));
  for (std::size_t i = 0; i < egresses.size(); ++i) {
    const etg::Egress::Stats es = egresses[i]->stats();
    const etg::QueueStats qs = queues[i]->stats();
    std::fprintf(stderr,
                 "consumer %-10s sent=%llu bytes=%llu batches=%llu dropped=%llu "
                 "would_block=%llu partial=%llu connects=%llu disconnects=%llu\n",
                 egresses[i]->name().c_str(), static_cast<unsigned long long>(es.frames_sent),
                 static_cast<unsigned long long>(es.bytes_sent),
                 static_cast<unsigned long long>(es.batches_sent),
                 static_cast<unsigned long long>(qs.dropped),
                 static_cast<unsigned long long>(es.would_block),
                 static_cast<unsigned long long>(es.partial_writes),
                 static_cast<unsigned long long>(es.connects),
                 static_cast<unsigned long long>(es.disconnects));
  }
  return 0;
}
