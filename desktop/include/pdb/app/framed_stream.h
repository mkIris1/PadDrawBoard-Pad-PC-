#pragma once

#include "paddraw_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <winsock2.h>

namespace pdb::app {

enum class IoStatus { kOk, kClosed, kWouldBlock, kError };

struct IoResult {
  IoStatus status{IoStatus::kError};
  std::size_t transferred{};
  std::string error;
};

class IByteStream {
 public:
  virtual ~IByteStream() = default;
  virtual IoResult Read(std::span<std::uint8_t> destination) = 0;
  virtual IoResult Write(std::span<const std::uint8_t> source) = 0;
};

struct FrameIoResult {
  IoStatus status{IoStatus::kError};
  std::size_t transferred{};
  std::string error;
};

// ReadFrame continues through short I/O. WriteFrame is a one-shot convenience
// API; if it returns after a partial write, the caller must tear down the
// stream because the peer can no longer be resynchronized. Code that owns a
// live stream across would-block must use ResumableFrameWriter instead.
class FramedStream final {
 public:
  [[nodiscard]] static FrameIoResult ReadFrame(IByteStream& stream,
                                               paddrawboard::protocol::Frame* frame);
  [[nodiscard]] static FrameIoResult WriteFrame(IByteStream& stream,
                                                const paddrawboard::protocol::Frame& frame);
};

// Owns one serialized protocol frame and its send offset. Continue() may be
// called again after kWouldBlock; it never starts another frame until this
// one is complete. A fatal result leaves the state intact for diagnostics, but
// the owning session must close the stream because its peer has a partial
// frame and cannot be resynchronized.
class ResumableFrameWriter final {
 public:
  [[nodiscard]] FrameIoResult Start(const paddrawboard::protocol::Frame& frame);
  [[nodiscard]] FrameIoResult Continue(IByteStream& stream);
  [[nodiscard]] bool pending() const noexcept { return !bytes_.empty(); }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  void Clear() noexcept {
    bytes_.clear();
    payload_scratch_.clear();
    offset_ = 0;
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::vector<std::uint8_t> payload_scratch_;
  std::size_t offset_{};
};

class SocketByteStream final : public IByteStream {
 public:
  explicit SocketByteStream(SOCKET socket) : socket_(socket) {}

  IoResult Read(std::span<std::uint8_t> destination) override;
  IoResult Write(std::span<const std::uint8_t> source) override;

  [[nodiscard]] static bool SetTimeouts(SOCKET socket, int receive_ms,
                                        int send_ms) noexcept;

 private:
  SOCKET socket_{INVALID_SOCKET};  // Non-owning; DesktopServer owns closure.
};

}  // namespace pdb::app
