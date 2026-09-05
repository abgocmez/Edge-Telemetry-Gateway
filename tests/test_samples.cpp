#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "samples.hpp"

using namespace etg;

TEST_CASE("percentiles over a known distribution", "[samples]") {
  Samples s;
  for (int i = 1; i <= 100; ++i) {
    s.add(i);
  }

  const Percentiles p = s.compute();
  CHECK(p.count == 100);
  CHECK(s.retained() == 100);
  CHECK(s.stride() == 1);  // nothing was decimated
  CHECK(p.min == 1);
  CHECK(p.max == 100);
  CHECK(p.mean == 50.5);
  // Nearest rank: index ceil(p*n) - 1 of the sorted samples.
  CHECK(p.p50 == 50);
  CHECK(p.p90 == 90);
  CHECK(p.p99 == 99);
}

TEST_CASE("insertion order does not affect the result", "[samples]") {
  Samples ascending;
  Samples descending;
  for (int i = 1; i <= 1000; ++i) {
    ascending.add(i);
    descending.add(1001 - i);
  }
  const Percentiles a = ascending.compute();
  const Percentiles d = descending.compute();
  CHECK(a.p50 == d.p50);
  CHECK(a.p99 == d.p99);
  CHECK(a.max == d.max);
}

TEST_CASE("negative pacing error is preserved", "[samples]") {
  // A tick that fires early is a real observation, not an error to clamp away.
  Samples s;
  for (int i = -50; i <= 50; ++i) {
    s.add(i);
  }
  const Percentiles p = s.compute();
  CHECK(p.min == -50);
  CHECK(p.max == 50);
  CHECK(p.p50 == 0);
}

TEST_CASE("an empty buffer reports zeros rather than reading past the end", "[samples]") {
  const Samples s;
  const Percentiles p = s.compute();
  CHECK(p.count == 0);
  CHECK(p.min == 0);
  CHECK(p.max == 0);
  CHECK(p.p99 == 0);
}

TEST_CASE("decimation bounds memory and keeps the distribution honest", "[samples]") {
  // Capacity far below the sample count, so decimation must trigger repeatedly.
  Samples s{1024};
  constexpr int kN = 100'000;
  for (int i = 1; i <= kN; ++i) {
    s.add(i);
  }

  CHECK(s.total() == kN);
  CHECK(s.retained() <= 1024);
  CHECK(s.stride() > 1);

  const Percentiles p = s.compute();
  CHECK(p.count == kN);

  // Systematic decimation keeps order statistics close to the truth. Keeping
  // the first N would report p50 near 512, and keeping the last N would report
  // it near 99'500; both would fail this bound by two orders of magnitude.
  const double err = std::abs(static_cast<double>(p.p50) - 50'000.0) / 50'000.0;
  CHECK(err < 0.05);
  CHECK(p.max > 90'000);
}

TEST_CASE("a zero capacity does not divide by zero or spin", "[samples]") {
  Samples s{0};
  for (int i = 0; i < 100; ++i) {
    s.add(i);
  }
  CHECK(s.total() == 100);
  CHECK(s.retained() <= 1);
}
