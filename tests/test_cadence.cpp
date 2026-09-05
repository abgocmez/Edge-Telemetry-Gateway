#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "cadence.hpp"
#include "wire.hpp"

using namespace etg;

namespace {

constexpr std::uint64_t kPeriodNs = 10'000'000;  // 10 ms, a typical ECU cadence

Frame tick(std::uint32_t can_id, std::uint64_t seq, std::uint64_t t_ns) {
  Frame f{};
  f.seq = seq;
  f.can_id = can_id;
  f.t_ingest_ns = t_ns;
  f.len = 8;
  return f;
}

}  // namespace

TEST_CASE("a steady id produces no anomalies once its period is learned", "[cadence]") {
  CadenceMonitor m{8, 4.0, true};
  for (std::uint64_t i = 0; i < 200; ++i) {
    CHECK_FALSE(m.observe(tick(0x100, i, i * kPeriodNs)));
  }
  CHECK(m.frames() == 200);
  CHECK(m.anomalies() == 0);
  CHECK(m.per_id().at(0x100).expected_ns == kPeriodNs);
}

TEST_CASE("an id that goes quiet is flagged", "[cadence]") {
  CadenceMonitor m{8, 4.0, true};
  std::uint64_t t = 0;
  for (std::uint64_t i = 0; i < 50; ++i) {
    m.observe(tick(0x100, i, t));
    t += kPeriodNs;
  }
  CHECK(m.anomalies() == 0);

  // The ECU stops for half a second, then resumes. That is the fault this
  // consumer exists to notice.
  t += 500 * kPeriodNs;
  CHECK(m.observe(tick(0x100, 50, t)));
  CHECK(m.anomalies() == 1);
}

TEST_CASE("a small wobble is not flagged", "[cadence]") {
  CadenceMonitor m{8, 4.0, true};
  std::uint64_t t = 0;
  for (std::uint64_t i = 0; i < 100; ++i) {
    // +/-20% jitter, well inside the factor of four.
    const std::uint64_t step = kPeriodNs + (i % 5) * (kPeriodNs / 25);
    m.observe(tick(0x100, i, t));
    t += step;
  }
  CHECK(m.anomalies() == 0);
}

TEST_CASE("loss the consumer caused is not reported as a bus fault", "[cadence][gap]") {
  // The whole argument for gap markers, as a test rather than a demonstration.
  //
  // Two monitors, the same input, differing only in whether they believe the
  // gateway when it says frames were lost. The bus never faltered: every frame
  // that exists is exactly one period after the last. What happened is that this
  // consumer missed a run of them.
  auto run = [](bool gap_aware) {
    CadenceMonitor m{8, 4.0, gap_aware};
    std::uint64_t seq = 0;
    std::uint64_t t = 0;

    const auto steady = [&](int n) {
      for (int i = 0; i < n; ++i) {
        m.observe(tick(0x100, seq++, t));
        t += kPeriodNs;
      }
    };

    steady(40);  // learn the period

    for (int episode = 0; episode < 5; ++episode) {
      // 100 frames vanish. The gateway says so.
      const std::uint64_t lost = 100;
      m.observe(wire::make_gap(seq, lost, GapReason::kConsumerOverrun, t));
      seq += lost;
      t += lost * kPeriodNs;

      steady(20);
    }
    return m;
  };

  const CadenceMonitor aware = run(true);
  const CadenceMonitor blind = run(false);

  CHECK(aware.frames() == blind.frames());
  CHECK(aware.markers() == 5);
  CHECK(blind.markers() == 5);
  CHECK(aware.reported_lost() == 500);

  // Blind: every resumption after a loss looks like an id that was silent for a
  // second. Five episodes, five false alarms.
  CHECK(blind.anomalies() == 5);
  CHECK(blind.suppressed() == 0);

  // Aware: the gateway said what happened, so the interval that spans the hole
  // is not measured at all. No false alarms, and nothing swept under the rug -
  // the suppressions are counted and reported.
  CHECK(aware.anomalies() == 0);
  CHECK(aware.suppressed() == 5);
}

TEST_CASE("a real fault is still caught while gaps are being suppressed",
          "[cadence][gap]") {
  // Suppression must not become a blanket excuse: after resynchronising, the
  // very next interval is checked again. A consumer that stopped reporting
  // faults whenever it was busy would be worse than useless.
  CadenceMonitor m{8, 4.0, true};
  std::uint64_t seq = 0;
  std::uint64_t t = 0;

  for (int i = 0; i < 40; ++i) {
    m.observe(tick(0x100, seq++, t));
    t += kPeriodNs;
  }

  m.observe(wire::make_gap(seq, 100, GapReason::kConsumerOverrun, t));
  seq += 100;
  t += 100 * kPeriodNs;

  // First frame after the gap: the interval spans the hole, so it is skipped.
  CHECK_FALSE(m.observe(tick(0x100, seq++, t)));
  t += kPeriodNs;
  CHECK_FALSE(m.observe(tick(0x100, seq++, t)));

  // Now the ECU genuinely stops, with no loss to blame it on.
  t += 500 * kPeriodNs;
  CHECK(m.observe(tick(0x100, seq++, t)));
  CHECK(m.anomalies() == 1);
  CHECK(m.suppressed() == 1);
}

TEST_CASE("a marker suppresses every id, not only the ones seen nearby",
          "[cadence][gap]") {
  // A marker says how many sequences were lost, never which ids they carried.
  // An id that only appears occasionally is exactly the one a cadence monitor is
  // watching, and exactly the one that would be falsely flagged if suppression
  // were limited to ids seen in the surrounding batches.
  CadenceMonitor m{4, 4.0, true};
  std::uint64_t seq = 0;
  std::uint64_t t = 0;

  // Two ids, one frequent and one rare, both learned.
  for (int i = 0; i < 40; ++i) {
    m.observe(tick(0x100, seq++, t));
    if (i % 10 == 0) {
      m.observe(tick(0x200, seq++, t));
    }
    t += kPeriodNs;
  }

  m.observe(wire::make_gap(seq, 200, GapReason::kConsumerOverrun, t));
  seq += 200;
  t += 200 * kPeriodNs;

  // The rare id reappears after the hole. Its interval is enormous, and it is
  // not an anomaly.
  CHECK_FALSE(m.observe(tick(0x200, seq++, t)));
  CHECK(m.anomalies() == 0);
  CHECK(m.per_id().at(0x200).suppressed == 1);
}

TEST_CASE("a reconnect is treated as a discontinuity for every id", "[cadence]") {
  CadenceMonitor m{8, 4.0, true};
  std::uint64_t t = 0;
  for (std::uint64_t i = 0; i < 40; ++i) {
    m.observe(tick(0x100, i, t));
    t += kPeriodNs;
  }

  // The consumer was away for a while; the stream moved on without it.
  m.reset_all();
  t += 1000 * kPeriodNs;

  CHECK_FALSE(m.observe(tick(0x100, 5000, t)));
  CHECK(m.anomalies() == 0);
  CHECK(m.suppressed() == 1);
}
