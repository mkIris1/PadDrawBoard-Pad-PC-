#include "pdb/app/framed_stream.h"

#include <algorithm>
#include <array>
#include <climits>
#include <exception>
#include <limits>
#include <vector>

namespace pdb::app {
namespace {

constexpr std::size_t kPayloadLengthOffset = 16;

std::uint32_t ReadLe32(const std::uint8_t* bytes) noexcept {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

FrameIoResult ReadExact(IByteStream& stream, std::span<std::uint8_t> destination) {
  std::size_t total{};
  while (total < destination.size()) {
    const IoResult result = stream.Read(destination.subspan(total));
    if (result.transferred > destination.size() - total) {
      return {IoStatus::kError, total, "stream returned an invalid read length"};
    }
    total += result.transferred;
    if (result.status != IoStatus::kOk) return {result.status, total, result.error};
    if (result.transferred == 0) return {IoStatus::kClosed, total, "peer closed during frame read"};
  }
  return {IoStatus::kOk, total, {}};
}

IoStatus SocketStatusForError(int error) noexcept {
  switch (error) {
    case WSAEWOULDBLOCK:
    case WSAETIMEDOUT:
      return IoStatus::kWouldBlock;
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENOTCONN:
    case WSAESHUTDOWN:
      return IoStatus::kClosed;
    default:
      return IoStatus::kError;
  }
}

IoResult SocketFailure(const char* operation) {
  const int error = WSAGetLastError();
  return {SocketStatusForError(error), 0,
          std::string(operation) + " failed: WSA " + std::to_string(error)};
}

}  // namespace

FrameIoResult FramedStream::ReadFrame(IByteStream& stream,
                                      paddrawboard::protocol::Frame* frame) {
  if (frame == nullptr) return {IoStatus::kError, 0, "null protocol frame result"};
  std::array<std::uint8_t, paddrawboard::protocol::kFrameHeaderBytes> header{};
  FrameIoResult result = ReadExact(stream, header);
  if (result.status != IoStatus::kOk) return result;
  const std::uint32_t payload_length = ReadLe32(header.data() + kPayloadLengthOffset);
  if (payload_length > paddrawboard::protocol::kMaxPayloadBytes) {
    return {IoStatus::kError, result.transferred, "protocol payload exceeds v1 maximum"};
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(header.size() + payload_length);
  bytes.insert(bytes.end(), header.begin(), header.end());
  bytes.resize(header.size() + payload_length);
  if (payload_length != 0) {
    FrameIoResult body = ReadExact(stream, std::span<std::uint8_t>(bytes).subspan(header.size()));
    body.transferred += result.transferred;
    if (body.status != IoStatus::kOk) return body;
    result = body;
  }
  const auto decoded = paddrawboard::protocol::decodeFrame(bytes);
  if (const auto* error = std::get_if<paddrawboard::protocol::ParseError>(&decoded)) {
    return {IoStatus::kError, result.transferred, "invalid protocol frame: " + error->message};
  }
  *frame = std::get<paddrawboard::protocol::Frame>(decoded);
  return {IoStatus::kOk, result.transferred, {}};
}

FrameIoResult FramedStream::WriteFrame(IByteStream& stream,
                                       const paddrawboard::protocol::Frame& frame) {
  ResumableFrameWriter writer;
  const FrameIoResult started = writer.Start(frame);
  if (started.status != IoStatus::kOk) return started;
  return writer.Continue(stream);
}

FrameIoResult ResumableFrameWriter::Start(const paddrawboard::protocol::Frame& frame) {
  if (pending()) return {IoStatus::kError, 0, "another protocol frame is already pending"};
  try {
    paddrawboard::protocol::encodeFrame(frame, bytes_, payload_scratch_);
    offset_ = 0;
    return {IoStatus::kOk, 0, {}};
  } catch (const std::exception& error) {
    Clear();
    return {IoStatus::kError, 0, "cannot encode protocol frame: " + std::string(error.what())};
  }
}

FrameIoResult ResumableFrameWriter::Continue(IByteStream& stream) {
  if (!pending()) return {IoStatus::kError, 0, "no protocol frame is pending"};
  const std::size_t starting_offset = offset_;
  while (offset_ < bytes_.size()) {
    const IoResult result = stream.Write(
        std::span<const std::uint8_t>(bytes_).subspan(offset_));
    if (result.transferred > bytes_.size() - offset_) {
      return {IoStatus::kError, offset_ - starting_offset,
              "stream returned an invalid write length"};
    }
    offset_ += result.transferred;
    if (result.status != IoStatus::kOk) {
      return {result.status, offset_ - starting_offset, result.error};
    }
    if (result.transferred == 0) {
      return {IoStatus::kClosed, offset_ - starting_offset,
              "peer closed during frame write"};
    }
  }
  const std::size_t transferred = offset_ - starting_offset;
  Clear();
  return {IoStatus::kOk, transferred, {}};
}

IoResult SocketByteStream::Read(std::span<std::uint8_t> destination) {
  if (socket_ == INVALID_SOCKET) return {IoStatus::kClosed, 0, "socket is closed"};
  if (destination.empty()) return {IoStatus::kOk, 0, {}};
  const int length = static_cast<int>(std::min<std::size_t>(destination.size(), INT_MAX));
  const int read = recv(socket_, reinterpret_cast<char*>(destination.data()), length, 0);
  if (read > 0) return {IoStatus::kOk, static_cast<std::size_t>(read), {}};
  if (read == 0) return {IoStatus::kClosed, 0, "peer closed socket"};
  return SocketFailure("recv");
}

IoResult SocketByteStream::Write(std::span<const std::uint8_t> source) {
  if (socket_ == INVALID_SOCKET) return {IoStatus::kClosed, 0, "socket is closed"};
  if (source.empty()) return {IoStatus::kOk, 0, {}};
  const int length = static_cast<int>(std::min<std::size_t>(source.size(), INT_MAX));
  const int written = send(socket_, reinterpret_cast<const char*>(source.data()), length, 0);
  if (written > 0) return {IoStatus::kOk, static_cast<std::size_t>(written), {}};
  if (written == 0) return {IoStatus::kClosed, 0, "peer closed socket"};
  return SocketFailure("send");
}

bool SocketByteStream::SetTimeouts(SOCKET socket, int receive_ms, int send_ms) noexcept {
  if (socket == INVALID_SOCKET || receive_ms < 0 || send_ms < 0) return false;
  return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&receive_ms), sizeof(receive_ms)) == 0 &&
         setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                    reinterpret_cast<const char*>(&send_ms), sizeof(send_ms)) == 0;
}

}  // namespace pdb::app
