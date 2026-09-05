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

// 2 because gap markers set a bit that version 1 required decoders to reject.
// The alternative - quietly reusing a reserved bit - would have turned every
// deployed v1 consumer into one that reads a loss marker as a CAN frame with a
// nonsense identifier.
inline constexpr std::uint8_t  kVersion    = 3;
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

// A marker occupies a record slot but is not a frame: `seq` names the first
// sequence the consumer did not receive, and the payload carries how many are
// missing. The next real frame therefore has sequence `seq + count`, which lets
// a consumer close its books without having to guess.
//
// Emitted by the gateway rather than inferred by the consumer, because only the
// gateway can say *why*. A consumer can see that sequences 100 to 149 never
// arrived; it cannot tell whether it was too slow or whether the kernel dropped
// them before they ever had a sequence at all - and those two facts call for
// completely different responses.
[[nodiscard]] Frame make_gap(std::uint64_t first_missing, std::uint64_t count, GapReason reason,
                             std::uint64_t t_ingest_ns) noexcept;

[[nodiscard]] bool is_gap(const Frame& f) noexcept;
[[nodiscard]] std::uint64_t gap_count(const Frame& f) noexcept;
[[nodiscard]] GapReason gap_reason(const Frame& f) noexcept;

// A round-trip probe. The gateway emits one periodically; a consumer sends it
// straight back, unchanged, on the same connection.
//
// This exists because one-way latency across two machines needs their clocks to
// agree, and measuring it revealed that they do not: on the pair this was built
// for, the consumer's host had never completed a single NTP exchange and the two
// realtime clocks differed by 1.19 seconds. An echo needs no agreement at all -
// it is timed by one clock, on one machine, from send to return.
//
// It travels in-band, through the same batching and the same socket as the
// frames around it, so what it measures is what a frame experiences rather than
// what an idle side-channel would.
//
// The honest caveats, both unavoidable: halving assumes the path is symmetric,
// and the consumer's turnaround time is included in the figure.
[[nodiscard]] Frame make_echo(std::uint64_t nonce, std::uint64_t t_sent_ns) noexcept;
[[nodiscard]] bool is_echo(const Frame& f) noexcept;
[[nodiscard]] std::uint64_t echo_nonce(const Frame& f) noexcept;
[[nodiscard]] std::uint64_t echo_sent_ns(const Frame& f) noexcept;

// True for anything that is not a bus frame: a consumer must not count these as
// traffic, timestamp them, or record them as data.
[[nodiscard]] bool is_control(const Frame& f) noexcept;

void encode_header(std::uint32_t count, HeaderBytes out) noexcept;
[[nodiscard]] DecodeError decode_header(ConstHeaderBytes in, BatchHeader& out) noexcept;

}  // namespace etg::wire
