#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "frame.hpp"

// Binary wire protocol. See docs/wire-format.md for the normative field table.
//
// Encoding and decoding is field by field through explicit little-endian
// helpers. It never casts a receive buffer to a Frame, even though the
// in-memory layout currently happens to match the wire layout. That
// coincidence is not load-bearing: the moment a consumer is allowed to cast,
// the format stops being a protocol and becomes a shared header, and the claim
// that another language could implement it stops being true.
namespace etg::wire {

inline constexpr std::uint8_t  kVersion    = 1;
inline constexpr std::size_t   kHeaderSize = 16;
inline constexpr std::size_t   kRecordSize = 40;
inline constexpr std::uint32_t kMaxCount   = 65535;  // sanity bound on a decoded batch

using FrameBytes       = std::span<std::byte, kRecordSize>;
using ConstFrameBytes  = std::span<const std::byte, kRecordSize>;
using HeaderBytes      = std::span<std::byte, kHeaderSize>;
using ConstHeaderBytes = std::span<const std::byte, kHeaderSize>;

// Precedes every batch. record_len and header_len are on the wire so a future
// reader can skip a batch it does not understand instead of misparsing it.
struct BatchHeader {
  std::uint32_t count;        // records that follow
  std::uint32_t payload_len;  // count * record_len
  std::uint16_t record_len;
  std::uint8_t  version;
  std::uint8_t  header_len;
};

// Every rejection reason is distinct so a consumer can count and report which
// one it hit, rather than logging "bad frame".
enum class DecodeError : std::uint8_t {
  kOk = 0,
  kBadMagic,
  kUnsupportedVersion,
  kBadHeaderLen,
  kBadRecordLen,
  kInconsistentLength,
  kCountTooLarge,
  kReservedFlagSet,
  kLengthOutOfRange,
};

[[nodiscard]] const char* to_string(DecodeError e) noexcept;

void encode_frame(const Frame& f, FrameBytes out) noexcept;
[[nodiscard]] DecodeError decode_frame(ConstFrameBytes in, Frame& out) noexcept;

void encode_header(std::uint32_t count, HeaderBytes out) noexcept;
[[nodiscard]] DecodeError decode_header(ConstHeaderBytes in, BatchHeader& out) noexcept;

}  // namespace etg::wire
