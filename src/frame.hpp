#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace etg {

// Flags carried in Frame::flags. Bits 3-7 are reserved and must be zero in
// wire version 1; a decoder rejects a record that sets them, so adding a flag
// later is a version bump rather than a silent misread.
namespace frame_flags {
inline constexpr std::uint8_t kExtendedId = 1U << 0U;  // 29-bit CAN identifier
inline constexpr std::uint8_t kRemote     = 1U << 1U;  // RTR
inline constexpr std::uint8_t kError      = 1U << 2U;  // CAN_ERR_FLAG was set
inline constexpr std::uint8_t kReservedMask = 0xF8U;   // bits 3-7
}  // namespace frame_flags

// One normalised bus frame. Fixed size, trivially copyable, no indirection: it
// is copied straight into a ring slot, so nothing here may own memory.
//
// 40 bytes. With the ring cell's 8-byte seqlock counter that is 48, padding to
// 64 — one cell per cache line on both Cortex-A53 and x86_64.
//
// The two timestamps deliberately live in different clock domains; see
// docs/wire-format.md and the clock strategy in SCOPE.md.
struct Frame {
  std::uint64_t seq;          // global sequence, assigned at ring-claim time
  std::uint64_t t_kernel_ns;  // CLOCK_REALTIME, from SO_TIMESTAMPING
  std::uint64_t t_ingest_ns;  // CLOCK_MONOTONIC, taken inside the source
  std::uint32_t can_id;       // 11-bit, or 29-bit when kExtendedId is set
  std::uint8_t  src_id;       // which bus; per-source FIFO is guaranteed per id
  std::uint8_t  len;          // 0..8 for classic CAN
  std::uint8_t  flags;
  std::uint8_t  reserved;     // explicit, never left to the compiler
  std::array<std::uint8_t, 8> data;
};

inline constexpr std::size_t kMaxPayload = 8;  // classic CAN only; CAN FD is a version bump

// The in-memory layout is part of the design, not an accident of this compiler.
// A field that moves breaks the build here rather than corrupting the protocol.
static_assert(std::is_standard_layout_v<Frame>);
static_assert(std::is_trivially_copyable_v<Frame>);  // required to memcpy into a ring slot
static_assert(sizeof(Frame) == 40);
static_assert(alignof(Frame) == 8);
static_assert(offsetof(Frame, seq)         == 0);
static_assert(offsetof(Frame, t_kernel_ns) == 8);
static_assert(offsetof(Frame, t_ingest_ns) == 16);
static_assert(offsetof(Frame, can_id)      == 24);
static_assert(offsetof(Frame, src_id)      == 28);
static_assert(offsetof(Frame, len)         == 29);
static_assert(offsetof(Frame, flags)       == 30);
static_assert(offsetof(Frame, reserved)    == 31);
static_assert(offsetof(Frame, data)        == 32);

}  // namespace etg
