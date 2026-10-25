#pragma once

#include <cstdint>
#include <string>

#include <winsock2.h>

namespace pdb::adb {

[[nodiscard]] bool IsLoopbackPeer(SOCKET socket) noexcept;
[[nodiscard]] bool SetSocketNonBlocking(SOCKET socket, bool enabled) noexcept;
void CloseSocket(SOCKET socket) noexcept;

class LoopbackListener final {
 public:
  LoopbackListener() = default;
  ~LoopbackListener();

  LoopbackListener(const LoopbackListener&) = delete;
  LoopbackListener& operator=(const LoopbackListener&) = delete;
  LoopbackListener(LoopbackListener&& other) noexcept;
  LoopbackListener& operator=(LoopbackListener&& other) noexcept;

  bool Open(std::uint16_t port, int backlog = 8, std::string* error = nullptr);
  void Close() noexcept;

  enum class AcceptStatus { Accepted, Timeout, Error };
  [[nodiscard]] AcceptStatus Accept(SOCKET* accepted_socket,
                                    int timeout_ms = 0,
                                    std::string* error = nullptr) const;

  [[nodiscard]] bool is_open() const noexcept { return socket_ != INVALID_SOCKET; }
  [[nodiscard]] SOCKET socket() const noexcept { return socket_; }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

 private:
  SOCKET socket_ = INVALID_SOCKET;
  std::uint16_t port_ = 0;
  bool winsock_started_ = false;
};

}  // namespace pdb::adb
