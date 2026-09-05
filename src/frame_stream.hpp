#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "frame.hpp"
#include "wire.hpp"

namespace etg {

// Consumer-side reader for the wire protocol.
//
// It is an incremental parser holding its own partial state, because TCP is a
// byte stream and a read can land anywhere - halfway through a header, halfway
// through a record. Assuming one read yields one message is the single most
// common way to write a consumer that works in testing and corrupts under load,
// which is exactly when the batches get large enough to be split.
//
// Records are decoded field by field through the wire helpers. The receive
// buffer is never cast to a Frame, even though the layouts match today.
class FrameStream {
 public:
  enum class Status : std::uint8_t {
    kOk,             // a complete batch was decoded
    kTimeout,        // nothing complete yet; call again, partial state is kept
    kClosed,         // the peer closed, or the connection failed
    kProtocolError,  // the bytes were not our protocol; see last_error()
  };

  static std::unique_ptr<FrameStream> connect(const std::string& host, std::uint16_t port,
                                              std::string& error);

  ~FrameStream();

  FrameStream(const FrameStream&) = delete;
  FrameStream& operator=(const FrameStream&) = delete;

  Status read_batch(std::vector<Frame>& out, int timeout_ms);

  // Sends one record back to the gateway on the same connection.
  //
  // Used only to return echo probes. A reply that cannot be written immediately
  // is dropped rather than waited on: blocking a consumer's read loop to answer
  // a measurement probe would distort the very thing being measured, and a lost
  // probe costs one sample. Returns false in that case, and `echo_drops()`
  // counts them so a run with many of them is not mistaken for a clean one.
  bool send_back(const Frame& f);

  [[nodiscard]] std::uint64_t echo_drops() const noexcept { return echo_drops_; }

  [[nodiscard]] wire::DecodeError last_error() const noexcept { return last_error_; }
  [[nodiscard]] int fd() const noexcept { return fd_; }

 private:
  explicit FrameStream(int fd);

  // Reads until `need` bytes are buffered, or the deadline passes.
  Status fill(std::size_t need, std::int64_t deadline_ms);

  enum class State : std::uint8_t { kHeader, kPayload };

  int fd_;
  State state_ = State::kHeader;
  std::vector<std::byte> buf_;
  std::size_t got_ = 0;
  std::size_t need_ = wire::kHeaderSize;
  std::uint32_t records_ = 0;
  wire::DecodeError last_error_ = wire::DecodeError::kOk;
  std::uint64_t echo_drops_ = 0;
};

}  // namespace etg
