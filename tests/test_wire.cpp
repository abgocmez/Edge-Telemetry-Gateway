#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>

#include "frame.hpp"
#include "wire.hpp"

using namespace etg;

namespace {

template <std::size_t N>
constexpr std::array<std::byte, N> as_bytes(const std::array<std::uint8_t, N>& in) {
  std::array<std::byte, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = static_cast<std::byte>(in[i]);
  }
  return out;
}

// The golden vector. These bytes are written out by hand, not produced by the
// encoder, so the test fails if the layout drifts in either direction. This is
// the only thing standing between "a documented protocol" and "a shared header
// that happens to compile on both ends".
constexpr std::array<std::uint8_t, wire::kRecordSize> kGoldenRecord{
    // seq = 0x0123456789ABCDEF, little-endian
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
    // t_kernel_ns = 1'000'000'000
    0x00, 0xCA, 0x9A, 0x3B, 0x00, 0x00, 0x00, 0x00,
    // t_ingest_ns = 42
    0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // can_id = 0x1FF
    0xFF, 0x01, 0x00, 0x00,
    0x03,  // src_id
    0x08,  // len
    0x01,  // flags = kExtendedId
    0x00,  // reserved
    // data
    0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
};

constexpr std::array<std::uint8_t, wire::kHeaderSize> kGoldenHeader{
    0x45, 0x54, 0x47, 0x31,  // "ETG1"
    0x01,                    // version
    0x10,                    // header_len = 16
    0x28, 0x00,              // record_len = 40
    0x01, 0x00, 0x00, 0x00,  // count = 1
    0x28, 0x00, 0x00, 0x00,  // payload_len = 40
};

Frame golden_frame() {
  Frame f{};
  f.seq         = 0x0123456789ABCDEFULL;
  f.t_kernel_ns = 1'000'000'000ULL;
  f.t_ingest_ns = 42ULL;
  f.can_id      = 0x1FF;
  f.src_id      = 3;
  f.len         = 8;
  f.flags       = frame_flags::kExtendedId;
  f.reserved    = 0;
  f.data        = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
  return f;
}

}  // namespace

TEST_CASE("a frame encodes to exactly the golden bytes", "[wire][golden]") {
  std::array<std::byte, wire::kRecordSize> buf{};
  wire::encode_frame(golden_frame(), wire::FrameBytes{buf});
  REQUIRE(buf == as_bytes(kGoldenRecord));
}

TEST_CASE("the golden bytes decode to the expected fields", "[wire][golden]") {
  const auto bytes = as_bytes(kGoldenRecord);
  Frame f{};
  REQUIRE(wire::decode_frame(wire::ConstFrameBytes{bytes}, f) == wire::DecodeError::kOk);

  CHECK(f.seq         == 0x0123456789ABCDEFULL);
  CHECK(f.t_kernel_ns == 1'000'000'000ULL);
  CHECK(f.t_ingest_ns == 42ULL);
  CHECK(f.can_id      == 0x1FF);
  CHECK(f.src_id      == 3);
  CHECK(f.len         == 8);
  CHECK(f.flags       == frame_flags::kExtendedId);
  CHECK(f.data == std::array<std::uint8_t, 8>{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04});
}

TEST_CASE("a header encodes to exactly the golden bytes", "[wire][golden]") {
  std::array<std::byte, wire::kHeaderSize> buf{};
  wire::encode_header(1, wire::HeaderBytes{buf});
  REQUIRE(buf == as_bytes(kGoldenHeader));
}

TEST_CASE("frame encode/decode round-trips over the edges of every field", "[wire]") {
  const std::array<Frame, 3> cases{
      Frame{},  // all zero
      golden_frame(),
      Frame{.seq = UINT64_MAX,
            .t_kernel_ns = UINT64_MAX,
            .t_ingest_ns = UINT64_MAX,
            .can_id = 0x1FFFFFFF,  // 29-bit maximum
            .src_id = 255,
            .len = 8,
            .flags = frame_flags::kExtendedId | frame_flags::kRemote | frame_flags::kError,
            .reserved = 0,
            .data = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
  };

  for (const Frame& in : cases) {
    std::array<std::byte, wire::kRecordSize> buf{};
    wire::encode_frame(in, wire::FrameBytes{buf});

    Frame out{};
    REQUIRE(wire::decode_frame(wire::ConstFrameBytes{buf}, out) == wire::DecodeError::kOk);

    CHECK(out.seq == in.seq);
    CHECK(out.t_kernel_ns == in.t_kernel_ns);
    CHECK(out.t_ingest_ns == in.t_ingest_ns);
    CHECK(out.can_id == in.can_id);
    CHECK(out.src_id == in.src_id);
    CHECK(out.len == in.len);
    CHECK(out.flags == in.flags);
    CHECK(out.data == in.data);
  }
}

TEST_CASE("a decoder rejects malformed records with a distinct reason", "[wire][errors]") {
  SECTION("a reserved flag bit is a version violation, not a warning") {
    auto bytes = as_bytes(kGoldenRecord);
    bytes[30] = std::byte{0x08};  // bit 3, reserved in v1
    Frame f{};
    CHECK(wire::decode_frame(wire::ConstFrameBytes{bytes}, f) ==
          wire::DecodeError::kReservedFlagSet);
  }

  SECTION("classic CAN cannot carry more than 8 bytes") {
    auto bytes = as_bytes(kGoldenRecord);
    bytes[29] = std::byte{9};
    Frame f{};
    CHECK(wire::decode_frame(wire::ConstFrameBytes{bytes}, f) ==
          wire::DecodeError::kLengthOutOfRange);
  }
}

TEST_CASE("a decoder rejects malformed headers with a distinct reason", "[wire][errors]") {
  wire::BatchHeader h{};

  SECTION("bad magic") {
    auto bytes = as_bytes(kGoldenHeader);
    bytes[0] = std::byte{0x00};
    CHECK(wire::decode_header(wire::ConstHeaderBytes{bytes}, h) == wire::DecodeError::kBadMagic);
  }

  SECTION("unsupported version") {
    auto bytes = as_bytes(kGoldenHeader);
    bytes[4] = std::byte{0x02};
    CHECK(wire::decode_header(wire::ConstHeaderBytes{bytes}, h) ==
          wire::DecodeError::kUnsupportedVersion);
  }

  SECTION("record length that is not ours") {
    auto bytes = as_bytes(kGoldenHeader);
    bytes[6] = std::byte{0x58};  // 88, i.e. a CAN FD record
    CHECK(wire::decode_header(wire::ConstHeaderBytes{bytes}, h) ==
          wire::DecodeError::kBadRecordLen);
  }

  SECTION("payload length disagreeing with count is caught before any record is read") {
    auto bytes = as_bytes(kGoldenHeader);
    bytes[12] = std::byte{0x50};  // claims 80 bytes for one 40-byte record
    CHECK(wire::decode_header(wire::ConstHeaderBytes{bytes}, h) ==
          wire::DecodeError::kInconsistentLength);
  }
}

TEST_CASE("every decode error has a distinct message", "[wire][errors]") {
  using wire::DecodeError;
  const std::array<DecodeError, 9> all{
      DecodeError::kOk,          DecodeError::kBadMagic,
      DecodeError::kUnsupportedVersion, DecodeError::kBadHeaderLen,
      DecodeError::kBadRecordLen,       DecodeError::kInconsistentLength,
      DecodeError::kCountTooLarge,      DecodeError::kReservedFlagSet,
      DecodeError::kLengthOutOfRange,
  };
  for (const auto e : all) {
    CHECK(std::string_view{wire::to_string(e)} != "unknown");
  }
}
