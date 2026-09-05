// The gateway, in either topology.
//
//   --topology queue   (A)  one fan-out thread polls every source, assigns the
//                           sequence, and copies each frame into one bounded
//                           mutex queue per consumer. N copies per frame, and a
//                           single serialisation point at ingest - forced, not
//                           chosen: order in a queue is push order, so two
//                           ingest threads could hand a consumer sequences 6
//                           then 5.
//
//   --topology ring    (B)  one thread per source, all publishing into a shared
//                           lock-free broadcast ring. One copy per frame, no
//                           fan-out thread, no ingest serialisation - order
//                           comes from the cell position rather than from the
//                           order things were pushed.
//
// Both are real and both are kept. The point of building A first was never to
// throw it away: it is the baseline B has to beat, on identical work, with
// everything else in the process held constant.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "broadcast_ring.hpp"
#include "can_source.hpp"
#include "egress.hpp"
#include "fanout.hpp"
#include "feed.hpp"
#include "ring_ingest.hpp"
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
               "usage: etg-gateway [--source SPEC ...] [--consumer NAME:PORT ...]\n"
               "\n"
               "  --source can:vcan0     read a SocketCAN interface; repeatable\n"
               "  --source synth         generate frames in process; repeatable\n"
               "  --topology queue|ring  per-consumer mutex queues, or the shared\n"
               "                         lock-free broadcast ring (default: ring)\n"
               "  --consumer NAME:PORT   serve a consumer on PORT; repeatable\n"
               "  --capacity N           frames per queue, or ring cells (default 8192)\n"
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

std::unique_ptr<etg::Source> open_source(const std::string& spec, std::uint8_t src_id,
                                         std::uint32_t rate, std::string& error) {
  if (spec.rfind("can:", 0) == 0) {
    return etg::CanSource::open(spec.substr(4), src_id, /*receive_errors=*/true, error);
  }
  if (spec == "synth") {
    etg::SyntheticSource::Config cfg;
    cfg.src_id = src_id;
    cfg.period_ns = rate == 0 ? 1'000'000 : 1'000'000'000U / rate;
    if (cfg.period_ns == 0) {
      cfg.period_ns = 1;
    }
    return etg::SyntheticSource::open(cfg, error);
  }
  error = "unknown source: " + spec;
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> source_specs;
  std::vector<ConsumerSpec> consumers;
  std::string topology = "ring";
  std::size_t capacity = 8192;
  std::uint32_t rate = 1000;
  int seconds = 0;
  bool tap = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if (arg == "--source" && has_value) {
      source_specs.emplace_back(argv[++i]);
    } else if (arg == "--topology" && has_value) {
      topology = argv[++i];
    } else if (arg == "--consumer" && has_value) {
      ConsumerSpec c;
      if (!parse_consumer(argv[++i], c)) {
        std::fprintf(stderr, "bad consumer spec: %s (want NAME:PORT)\n", argv[i]);
        return 2;
      }
      consumers.push_back(c);
    } else if ((arg == "--capacity" || arg == "--queue") && has_value) {
      capacity = std::strtoul(argv[++i], nullptr, 10);
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

  if (topology != "queue" && topology != "ring") {
    std::fprintf(stderr, "unknown topology: %s (want queue or ring)\n\n", topology.c_str());
    usage();
    return 2;
  }
  if (capacity == 0 || rate == 0) {
    usage();
    return 2;
  }
  if (source_specs.empty()) {
    source_specs.emplace_back("synth");
  }
  if (consumers.empty() && !tap) {
    std::fprintf(stderr, "no consumers and no --tap: nothing to do\n\n");
    usage();
    return 2;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::string error;

  std::vector<std::unique_ptr<etg::Source>> sources;
  std::vector<etg::Source*> source_ptrs;
  for (std::size_t i = 0; i < source_specs.size(); ++i) {
    auto s = open_source(source_specs[i], static_cast<std::uint8_t>(i), rate, error);
    if (!s) {
      std::fprintf(stderr, "source %s: %s\n", source_specs[i].c_str(), error.c_str());
      return 1;
    }
    source_ptrs.push_back(s.get());
    sources.push_back(std::move(s));
  }

  // Declaration order is destruction order reversed, and that matters here: the
  // egress threads hold references to feeds, and ring feeds hold a reference to
  // the ring. Declaring them in this order means the egresses are torn down
  // first, before anything they point at goes away.
  auto ring = topology == "ring" ? std::make_unique<etg::BroadcastRing>(capacity) : nullptr;

  std::vector<std::unique_ptr<etg::ConsumerFeed>> feeds;
  std::unique_ptr<etg::ConsumerFeed> tap_feed;

  auto make_feed = [&]() -> std::unique_ptr<etg::ConsumerFeed> {
    if (ring) {
      return std::make_unique<etg::RingFeed>(*ring);
    }
    return std::make_unique<etg::QueueFeed>(capacity);
  };

  for (std::size_t i = 0; i < consumers.size(); ++i) {
    feeds.push_back(make_feed());
  }
  if (tap) {
    // The tap is just another consumer, so printing cannot slow ingest down: it
    // falls behind and drops like anything else.
    tap_feed = make_feed();
  }

  std::vector<std::unique_ptr<etg::Egress>> egresses;
  for (std::size_t i = 0; i < consumers.size(); ++i) {
    auto e = etg::Egress::create(consumers[i].name, *feeds[i], consumers[i].port, error);
    if (!e) {
      std::fprintf(stderr, "consumer %s: %s\n", consumers[i].name.c_str(), error.c_str());
      return 1;
    }
    egresses.push_back(std::move(e));
  }

  std::unique_ptr<etg::RingIngest> ring_ingest;
  std::unique_ptr<etg::FanOut> fan;

  if (ring) {
    ring_ingest = etg::RingIngest::create(source_ptrs, *ring, error);
    if (!ring_ingest) {
      std::fprintf(stderr, "ingest: %s\n", error.c_str());
      return 1;
    }
  } else {
    std::vector<etg::FrameQueue*> sinks;
    for (auto& f : feeds) {
      sinks.push_back(&dynamic_cast<etg::QueueFeed&>(*f).queue());
    }
    if (tap_feed) {
      sinks.push_back(&dynamic_cast<etg::QueueFeed&>(*tap_feed).queue());
    }
    fan = etg::FanOut::create(source_ptrs, sinks, error);
    if (!fan) {
      std::fprintf(stderr, "fan-out: %s\n", error.c_str());
      return 1;
    }
  }

  for (auto& e : egresses) {
    e->start();
    std::fprintf(stderr, "consumer %-10s listening on port %u\n", e->name().c_str(), e->port());
  }
  if (ring_ingest) {
    ring_ingest->start();
  } else {
    fan->start();
  }

  std::fprintf(stderr, "topology %s, %zu source(s), capacity %zu\n", topology.c_str(),
               source_ptrs.size(), ring ? ring->capacity() : capacity);
  for (const auto& s : sources) {
    std::fprintf(stderr, "  source %s\n", std::string{s->name()}.c_str());
  }

  const std::uint64_t deadline =
      seconds > 0 ? etg::monotonic_ns() + static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL
                  : UINT64_MAX;

  std::vector<etg::Frame> tap_batch(256);
  std::uint64_t next_report = etg::monotonic_ns() + 1'000'000'000ULL;
  std::uint64_t last_frames = 0;

  const auto ingested = [&]() -> std::uint64_t {
    return ring_ingest ? ring_ingest->stats().frames : fan->stats().frames;
  };

  while (g_stop == 0 && etg::monotonic_ns() < deadline) {
    if (tap_feed) {
      const std::size_t n =
          tap_feed->pop_batch(std::span<etg::Frame>{tap_batch}, std::chrono::milliseconds{100});
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
      const std::uint64_t total = ingested();
      std::fprintf(stderr, "ingest %llu/s total=%llu",
                   static_cast<unsigned long long>(total - last_frames),
                   static_cast<unsigned long long>(total));
      last_frames = total;
      for (std::size_t i = 0; i < egresses.size(); ++i) {
        const etg::Egress::Stats es = egresses[i]->stats();
        const etg::FeedStats fs = feeds[i]->stats();
        std::fprintf(stderr, " | %s %s sent=%llu drop=%llu wblock=%llu lag=%zu",
                     egresses[i]->name().c_str(), es.connected ? "up" : "--",
                     static_cast<unsigned long long>(es.frames_sent),
                     static_cast<unsigned long long>(fs.dropped),
                     static_cast<unsigned long long>(es.would_block), fs.backlog);
      }
      std::fprintf(stderr, "\n");
    }
  }

  std::fprintf(stderr, "\nshutting down\n");
  if (ring_ingest) {
    ring_ingest->stop();
  } else {
    fan->stop();
  }
  for (auto& e : egresses) {
    e->stop();
  }

  std::fprintf(stderr, "\ntopology %s\n", topology.c_str());
  for (const auto& s : sources) {
    const etg::SourceStats ss = s->stats();
    std::fprintf(stderr, "source   %-10s frames=%llu kernel_drops=%llu read_errors=%llu\n",
                 std::string{s->name()}.c_str(), static_cast<unsigned long long>(ss.frames),
                 static_cast<unsigned long long>(ss.kernel_drops),
                 static_cast<unsigned long long>(ss.read_errors));
  }

  if (ring_ingest) {
    const etg::RingIngest::Stats is = ring_ingest->stats();
    std::fprintf(stderr, "ingest   frames=%llu batches=%llu threads=%zu claim_retries=%llu\n",
                 static_cast<unsigned long long>(is.frames),
                 static_cast<unsigned long long>(is.batches), ring_ingest->source_count(),
                 static_cast<unsigned long long>(ring->claim_retries()));
  } else {
    const etg::FanOut::Stats is = fan->stats();
    std::fprintf(stderr, "ingest   frames=%llu batches=%llu threads=1\n",
                 static_cast<unsigned long long>(is.frames),
                 static_cast<unsigned long long>(is.batches));
  }

  for (std::size_t i = 0; i < egresses.size(); ++i) {
    const etg::Egress::Stats es = egresses[i]->stats();
    const etg::FeedStats fs = feeds[i]->stats();
    std::fprintf(stderr,
                 "consumer %-10s sent=%llu bytes=%llu batches=%llu dropped=%llu "
                 "would_block=%llu partial=%llu connects=%llu disconnects=%llu "
                 "gaps_sent=%llu frames_lost=%llu\n",
                 egresses[i]->name().c_str(), static_cast<unsigned long long>(es.frames_sent),
                 static_cast<unsigned long long>(es.bytes_sent),
                 static_cast<unsigned long long>(es.batches_sent),
                 static_cast<unsigned long long>(fs.dropped),
                 static_cast<unsigned long long>(es.would_block),
                 static_cast<unsigned long long>(es.partial_writes),
                 static_cast<unsigned long long>(es.connects),
                 static_cast<unsigned long long>(es.disconnects),
                 static_cast<unsigned long long>(es.gaps_sent),
                 static_cast<unsigned long long>(es.frames_lost));
  }
  return 0;
}
