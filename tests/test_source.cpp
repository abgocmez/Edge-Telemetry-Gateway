#include <net/if.h>
#include <poll.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <string>

#include "can_source.hpp"
#include "source.hpp"
#include "synthetic_source.hpp"

using namespace etg;

namespace {

// Waits for the source to become readable. Returns false on timeout, so a
// hung test fails with a clear message instead of blocking CI forever.
bool wait_readable(const Source& s, int timeout_ms) {
  struct pollfd pfd {};
  pfd.fd = s.fd();
  pfd.events = POLLIN;
  return ::poll(&pfd, 1, timeout_ms) == 1;
}

bool interface_exists(const char* name) { return ::if_nametoindex(name) != 0; }

}  // namespace

TEST_CASE("a synthetic source refuses a configuration it cannot honour", "[source][synth]") {
  std::string error;

  SECTION("no CAN ids") {
    SyntheticSource::Config cfg;
    cfg.can_ids.clear();
    CHECK(SyntheticSource::open(cfg, error) == nullptr);
    CHECK_FALSE(error.empty());
  }

  SECTION("zero period") {
    SyntheticSource::Config cfg;
    cfg.period_ns = 0;
    CHECK(SyntheticSource::open(cfg, error) == nullptr);
    CHECK_FALSE(error.empty());
  }
}

TEST_CASE("a synthetic source produces frames and fills every field but seq",
          "[source][synth]") {
  SyntheticSource::Config cfg;
  cfg.src_id = 7;
  cfg.period_ns = 200'000;  // 5 kHz
  cfg.frames_per_tick = 1;
  cfg.can_ids = {0x111, 0x222};

  std::string error;
  auto src = SyntheticSource::open(cfg, error);
  REQUIRE(src != nullptr);
  REQUIRE(error.empty());
  CHECK(src->id() == 7);
  CHECK(src->fd() >= 0);

  REQUIRE(wait_readable(*src, 1000));

  std::array<Frame, 16> batch{};
  const std::size_t n = src->drain(batch);
  REQUIRE(n > 0);

  for (std::size_t i = 0; i < n; ++i) {
    const Frame& f = batch[i];
    CHECK(f.src_id == 7);
    CHECK(f.len == 8);
    CHECK(f.t_ingest_ns > 0);
    CHECK(f.t_kernel_ns > 0);
    // seq belongs to the ring, not the source: a single source cannot know the
    // global order across buses.
    CHECK(f.seq == 0);
    CHECK((f.can_id == 0x111 || f.can_id == 0x222));
  }

  CHECK(src->stats().frames == n);
}

TEST_CASE("a synthetic source never writes past the span it was given",
          "[source][synth]") {
  SyntheticSource::Config cfg;
  cfg.period_ns = 100'000;
  cfg.frames_per_tick = 32;  // deliberately more than the drain buffer

  std::string error;
  auto src = SyntheticSource::open(cfg, error);
  REQUIRE(src != nullptr);
  REQUIRE(wait_readable(*src, 1000));

  std::array<Frame, 4> small{};
  const std::size_t n = src->drain(small);
  CHECK(n <= small.size());
}

TEST_CASE("a synthetic source cycles its CAN ids in order", "[source][synth]") {
  SyntheticSource::Config cfg;
  cfg.period_ns = 100'000;
  cfg.frames_per_tick = 8;
  cfg.can_ids = {0xA, 0xB, 0xC};

  std::string error;
  auto src = SyntheticSource::open(cfg, error);
  REQUIRE(src != nullptr);
  REQUIRE(wait_readable(*src, 1000));

  std::array<Frame, 8> batch{};
  const std::size_t n = src->drain(batch);
  REQUIRE(n >= 4);
  for (std::size_t i = 0; i < n; ++i) {
    CHECK(batch[i].can_id == cfg.can_ids[i % cfg.can_ids.size()]);
  }
}

TEST_CASE("opening a CAN source names the interface that failed", "[source][can]") {
  std::string error;
  auto src = CanSource::open("definitely-not-a-real-if", 0, false, error);
  CHECK(src == nullptr);
  CHECK(error.find("definitely-not-a-real-if") != std::string::npos);
}

// Requires a real vcan interface, so it runs in CI and on the Pi but is skipped
// on WSL2, whose kernel has CONFIG_CAN and CONFIG_CAN_RAW but not
// CONFIG_CAN_VCAN.
TEST_CASE("a CAN source receives a frame sent on vcan0", "[source][can][vcan]") {
  if (!interface_exists("vcan0")) {
    SKIP("vcan0 is not present; run scripts/setup-vcan.sh");
  }

  std::string error;
  auto src = CanSource::open("vcan0", 3, /*receive_errors=*/true, error);
  REQUIRE(src != nullptr);
  REQUIRE(error.empty());

  REQUIRE(std::system("cansend vcan0 1FF#DEADBEEF01020304") == 0);
  REQUIRE(wait_readable(*src, 2000));

  std::array<Frame, 16> batch{};
  const std::size_t n = src->drain(batch);
  REQUIRE(n >= 1);

  const Frame& f = batch[0];
  CHECK(f.can_id == 0x1FF);
  CHECK(f.src_id == 3);
  CHECK(f.len == 8);
  CHECK((f.flags & frame_flags::kExtendedId) == 0);
  CHECK(f.data == std::array<std::uint8_t, 8>{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04});

  // The point of enabling SO_TIMESTAMPING: without it this field is zero and
  // the kernel-to-userspace delta cannot be measured at all.
  CHECK(f.t_kernel_ns > 0);
  CHECK(f.t_ingest_ns > 0);
}

TEST_CASE("a CAN source reports an extended identifier as extended",
          "[source][can][vcan]") {
  if (!interface_exists("vcan0")) {
    SKIP("vcan0 is not present; run scripts/setup-vcan.sh");
  }

  std::string error;
  auto src = CanSource::open("vcan0", 0, false, error);
  REQUIRE(src != nullptr);

  REQUIRE(std::system("cansend vcan0 1FFFFFFF#0011") == 0);
  REQUIRE(wait_readable(*src, 2000));

  std::array<Frame, 16> batch{};
  const std::size_t n = src->drain(batch);
  REQUIRE(n >= 1);

  const Frame& f = batch[0];
  CHECK((f.flags & frame_flags::kExtendedId) != 0);
  CHECK(f.can_id == 0x1FFFFFFF);  // the flag bits must not leak into the id
  CHECK(f.len == 2);
}
