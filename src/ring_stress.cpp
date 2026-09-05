// Contention stress for the broadcast ring, outside the test framework.
//
// It exists to be run on the measurement target. The Catch2 suite needs
// FetchContent and a long build on four Cortex-A53 cores; this is one
// translation unit, so the ring can be exercised on real aarch64 hardware in
// the time it takes to type the command.
//
// That matters because of what aarch64 is for here. x86_64 is close to
// sequentially consistent for the accesses this structure makes: a release
// store and a relaxed store compile to the same instruction, and loads are not
// reordered with loads. Weakening the ordering in this ring therefore changes
// *nothing observable* on the development machine. On aarch64 the same edit
// changes `stlr` to `str` and the guarantee genuinely disappears.
//
// So a green run here on x86 proves the logic. Only a run on the Pi can say
// anything at all about the memory ordering, which is why the Pi is a required
// part of the plan rather than a nicer place to run the same tests.
//
// Two detectors, and they catch different failures:
//
//   torn          the payload is not internally consistent, so two producers
//                 interleaved their bytes into one cell. This is what the CAS
//                 claim prevents.
//
//   inconsistent  the payload is perfectly consistent but belongs to a
//                 different position than the sequence word claimed. This is
//                 what a weakened publish produces: the reader sees the new
//                 sequence before the new payload and copies out the previous
//                 occupant - a whole, valid, wrong frame that no checksum over
//                 the payload could ever notice.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "broadcast_ring.hpp"

namespace {

constexpr std::uint64_t kTearKey = 0x5A5AA5A5DEADBEEFULL;

etg::Frame make_frame(std::uint8_t producer, std::uint64_t counter) {
  etg::Frame f{};
  f.t_ingest_ns = counter;
  f.t_kernel_ns = counter ^ kTearKey;
  f.can_id = static_cast<std::uint32_t>(counter & 0x7FFU);
  f.src_id = producer;
  f.len = 8;
  for (std::size_t i = 0; i < 8; ++i) {
    f.data[i] = static_cast<std::uint8_t>((counter >> (8U * i)) & 0xFFU);
  }
  return f;
}

bool intact(const etg::Frame& f) {
  const std::uint64_t c = f.t_ingest_ns;
  if (f.t_kernel_ns != (c ^ kTearKey)) return false;
  if (f.can_id != static_cast<std::uint32_t>(c & 0x7FFU)) return false;
  for (std::size_t i = 0; i < 8; ++i) {
    if (f.data[i] != static_cast<std::uint8_t>((c >> (8U * i)) & 0xFFU)) return false;
  }
  return true;
}

void usage() {
  std::fprintf(stderr,
               "etg-ringstress - contention stress for the broadcast ring\n"
               "\n"
               "  --producers N   writer threads (default 4)\n"
               "  --readers N     reader threads (default 2)\n"
               "  --capacity N    ring cells, rounded up to a power of two (default 16)\n"
               "  --seconds N     run duration (default 5)\n"
               "\n"
               "A small capacity and more producers than cores is what makes cells\n"
               "collide; a large one makes the ring quiet. Exit status is non-zero if\n"
               "any frame was torn or read out of position.\n");
}

}  // namespace

int main(int argc, char** argv) {
  unsigned producers = 4;
  unsigned readers = 2;
  std::size_t capacity = 16;
  int seconds = 5;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if (arg == "--producers" && has_value) {
      producers = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--readers" && has_value) {
      readers = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--capacity" && has_value) {
      capacity = std::strtoul(argv[++i], nullptr, 10);
    } else if (arg == "--seconds" && has_value) {
      seconds = std::atoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n\n", arg.c_str());
      usage();
      return 2;
    }
  }
  if (producers == 0 || readers == 0 || capacity < 2 || seconds <= 0) {
    usage();
    return 2;
  }

  etg::BroadcastRing ring{capacity};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> torn{0};
  std::atomic<std::uint64_t> received{0};
  std::atomic<std::uint64_t> missed{0};
  std::atomic<std::uint64_t> regressions{0};

  std::vector<std::thread> writers;
  writers.reserve(producers);
  for (unsigned p = 0; p < producers; ++p) {
    writers.emplace_back([&, p] {
      while (!go.load(std::memory_order_acquire)) {
      }
      std::uint64_t i = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        ring.publish(make_frame(static_cast<std::uint8_t>(p & 0xFFU), i++));
      }
    });
  }

  std::vector<std::thread> reader_threads;
  reader_threads.reserve(readers);
  for (unsigned r = 0; r < readers; ++r) {
    reader_threads.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      etg::BroadcastRing::Cursor c;
      std::vector<etg::Frame> buf(32);
      std::uint64_t local_recv = 0;
      std::uint64_t local_torn = 0;
      std::uint64_t local_regress = 0;
      std::uint64_t last = 0;
      bool have = false;

      while (!stop.load(std::memory_order_relaxed)) {
        const etg::BroadcastRing::ReadResult res = ring.read_batch(c, buf);
        for (std::size_t i = 0; i < res.count; ++i) {
          ++local_recv;
          if (!intact(buf[i])) {
            ++local_torn;
          }
          // A reader's own view must never go backwards, lapped or not.
          if (have && buf[i].seq <= last) {
            ++local_regress;
          }
          last = buf[i].seq;
          have = true;
        }
      }
      received.fetch_add(local_recv, std::memory_order_relaxed);
      torn.fetch_add(local_torn, std::memory_order_relaxed);
      regressions.fetch_add(local_regress, std::memory_order_relaxed);
      missed.fetch_add(c.missed, std::memory_order_relaxed);
    });
  }

  const auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop.store(true, std::memory_order_relaxed);

  for (auto& t : writers) t.join();
  for (auto& t : reader_threads) t.join();
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  const std::uint64_t published = ring.write_position();
  const std::uint64_t bad_order = ring.inconsistent_reads();

  std::printf(
      "producers=%u readers=%u capacity=%zu seconds=%d\n"
      "\n"
      "published      %llu  (%.0f/s)\n"
      "received       %llu\n"
      "missed         %llu\n"
      "claim_retries  %llu\n"
      "\n"
      "torn           %llu   (two producers in one cell)\n"
      "inconsistent   %llu   (payload from the wrong position)\n"
      "regressions    %llu   (a reader's sequence went backwards)\n",
      producers, readers, capacity, seconds,
      static_cast<unsigned long long>(published),
      elapsed > 0 ? static_cast<double>(published) / elapsed : 0.0,
      static_cast<unsigned long long>(received.load()),
      static_cast<unsigned long long>(missed.load()),
      static_cast<unsigned long long>(ring.claim_retries()),
      static_cast<unsigned long long>(torn.load()),
      static_cast<unsigned long long>(bad_order),
      static_cast<unsigned long long>(regressions.load()));

  const bool failed = torn.load() != 0 || bad_order != 0 || regressions.load() != 0;
  std::printf("\n%s\n", failed ? "FAIL" : "OK");
  return failed ? 1 : 0;
}
