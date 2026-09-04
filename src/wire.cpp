#include "wire.hpp"

#include <array>

namespace etg::wire {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{0x45, 0x54, 0x47, 0x31};  // "ETG1"

// Written with shifts rather than a memcpy of the native representation, so the
// byte order is specified by this code instead of inherited from the host. On a
// little-endian target the compiler folds each of these back into a single
// store; the cost is zero and the guarantee is explicit.
template <typename T>
constexpr void store_le(T value, std::byte* out) noexcept {
  static_assert(std::is_unsigned_v<T>, "little-endian helpers take unsigned types");
  const auto v = static_cast<std::uint64_t>(value);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out[i] = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
  }
}

template <typename T>
[[nodiscard]] constexpr T load_le(const std::byte* in) noexcept {
  static_assert(std::is_unsigned_v<T>, "little-endian helpers take unsigned types");
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[i])) << (8U * i);
  }
  return static_cast<T>(v);
}

}  // namespace

const char* to_string(DecodeError e) noexcept {
  switch (e) {
    case DecodeError::kOk:                 return "ok";
    case DecodeError::kBadMagic:           return "bad magic";
    case DecodeError::kUnsupportedVersion: return "unsupported version";
    case DecodeError::kBadHeaderLen:       return "bad header length";
    case DecodeError::kBadRecordLen:       return "bad record length";
    case DecodeError::kInconsistentLength: return "payload length disagrees with count";
    case DecodeError::kCountTooLarge:      return "batch count too large";
    case DecodeError::kReservedFlagSet:    return "reserved flag bit set";
    case DecodeError::kLengthOutOfRange:   return "payload length out of range";
  }
  return "unknown";
}

void encode_frame(const Frame& f, FrameBytes out) noexcept {
  std::byte* p = out.data();
  store_le<std::uint64_t>(f.seq,         p + 0);
  store_le<std::uint64_t>(f.t_kernel_ns, p + 8);
  store_le<std::uint64_t>(f.t_ingest_ns, p + 16);
  store_le<std::uint32_t>(f.can_id,      p + 24);
  store_le<std::uint8_t>(f.src_id,       p + 28);
  store_le<std::uint8_t>(f.len,          p + 29);
  store_le<std::uint8_t>(f.flags,        p + 30);
  store_le<std::uint8_t>(f.reserved,     p + 31);
  for (std::size_t i = 0; i < kMaxPayload; ++i) {
    out[32 + i] = static_cast<std::byte>(f.data[i]);
  }
}

DecodeError decode_frame(ConstFrameBytes in, Frame& out) noexcept {
  const std::byte* p = in.data();

  const auto flags = load_le<std::uint8_t>(p + 30);
  if ((flags & frame_flags::kReservedMask) != 0U) {
    return DecodeError::kReservedFlagSet;
  }

  const auto len = load_le<std::uint8_t>(p + 29);
  if (len > kMaxPayload) {
    return DecodeError::kLengthOutOfRange;
  }

  out.seq         = load_le<std::uint64_t>(p + 0);
  out.t_kernel_ns = load_le<std::uint64_t>(p + 8);
  out.t_ingest_ns = load_le<std::uint64_t>(p + 16);
  out.can_id      = load_le<std::uint32_t>(p + 24);
  out.src_id      = load_le<std::uint8_t>(p + 28);
  out.len         = len;
  out.flags       = flags;
  out.reserved    = load_le<std::uint8_t>(p + 31);
  for (std::size_t i = 0; i < kMaxPayload; ++i) {
    out.data[i] = std::to_integer<std::uint8_t>(in[32 + i]);
  }
  return DecodeError::kOk;
}

void encode_header(std::uint32_t count, HeaderBytes out) noexcept {
  std::byte* p = out.data();
  for (std::size_t i = 0; i < kMagic.size(); ++i) {
    p[i] = static_cast<std::byte>(kMagic[i]);
  }
  store_le<std::uint8_t>(kVersion,                            p + 4);
  store_le<std::uint8_t>(static_cast<std::uint8_t>(kHeaderSize), p + 5);
  store_le<std::uint16_t>(static_cast<std::uint16_t>(kRecordSize), p + 6);
  store_le<std::uint32_t>(count,                              p + 8);
  store_le<std::uint32_t>(count * static_cast<std::uint32_t>(kRecordSize), p + 12);
}

DecodeError decode_header(ConstHeaderBytes in, BatchHeader& out) noexcept {
  const std::byte* p = in.data();

  for (std::size_t i = 0; i < kMagic.size(); ++i) {
    if (std::to_integer<std::uint8_t>(p[i]) != kMagic[i]) {
      return DecodeError::kBadMagic;
    }
  }

  const auto version    = load_le<std::uint8_t>(p + 4);
  const auto header_len = load_le<std::uint8_t>(p + 5);
  const auto record_len = load_le<std::uint16_t>(p + 6);
  const auto count      = load_le<std::uint32_t>(p + 8);
  const auto payload    = load_le<std::uint32_t>(p + 12);

  if (version != kVersion)                    return DecodeError::kUnsupportedVersion;
  if (header_len != kHeaderSize)              return DecodeError::kBadHeaderLen;
  if (record_len != kRecordSize)              return DecodeError::kBadRecordLen;
  if (count > kMaxCount)                      return DecodeError::kCountTooLarge;
  if (payload != count * static_cast<std::uint32_t>(kRecordSize)) {
    return DecodeError::kInconsistentLength;
  }

  out.count       = count;
  out.payload_len = payload;
  out.record_len  = record_len;
  out.version     = version;
  out.header_len  = header_len;
  return DecodeError::kOk;
}

}  // namespace etg::wire
