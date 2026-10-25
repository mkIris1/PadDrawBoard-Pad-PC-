#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "pdb/adb/loopback.h"

#include <cstring>

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace pdb::adb {
namespace {

void SetError(std::string* error, const char* operation) {
  if (error == nullptr) return;
  *error = std::string(operation) + " failed: " +
           std::to_string(WSAGetLastError());
}

}  // namespace

void CloseSocket(SOCKET socket) noexcept {
  if (socket != INVALID_SOCKET) closesocket(socket);
}

bool IsLoopbackPeer(SOCKET socket) noexcept {
  sockaddr_storage address{};
  int address_length = sizeof(address);
  if (getpeername(socket, reinterpret_cast<sockaddr*>(&address),
                  &address_length) == SOCKET_ERROR) {
    return false;
  }
  if (address.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
    return (ntohl(ipv4->sin_addr.s_addr) >> 24) == 127;
  }
  if (address.ss_family == AF_INET6) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
    static constexpr IN6_ADDR loopback = IN6ADDR_LOOPBACK_INIT;
    return std::memcmp(&ipv6->sin6_addr, &loopback, sizeof(loopback)) == 0;
  }
  return false;
}

bool SetSocketNonBlocking(SOCKET socket, bool enabled) noexcept {
  u_long value = enabled ? 1UL : 0UL;
  return ioctlsocket(socket, FIONBIO, &value) == 0;
}

LoopbackListener::~LoopbackListener() { Close(); }

LoopbackListener::LoopbackListener(LoopbackListener&& other) noexcept
    : socket_(other.socket_),
      port_(other.port_),
      winsock_started_(other.winsock_started_) {
  other.socket_ = INVALID_SOCKET;
  other.port_ = 0;
  other.winsock_started_ = false;
}

LoopbackListener& LoopbackListener::operator=(LoopbackListener&& other) noexcept {
  if (this == &other) return *this;
  Close();
  socket_ = other.socket_;
  port_ = other.port_;
  winsock_started_ = other.winsock_started_;
  other.socket_ = INVALID_SOCKET;
  other.port_ = 0;
  other.winsock_started_ = false;
  return *this;
}

bool LoopbackListener::Open(std::uint16_t port, int backlog, std::string* error) {
  Close();
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    SetError(error, "WSAStartup");
    return false;
  }
  winsock_started_ = true;
  socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_ == INVALID_SOCKET) {
    SetError(error, "socket");
    Close();
    return false;
  }
  const BOOL exclusive = TRUE;
  if (setsockopt(socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive),
                 sizeof(exclusive)) == SOCKET_ERROR) {
    SetError(error, "setsockopt SO_EXCLUSIVEADDRUSE");
    Close();
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(socket_, reinterpret_cast<const sockaddr*>(&address),
           sizeof(address)) == SOCKET_ERROR) {
    SetError(error, "bind loopback");
    Close();
    return false;
  }
  if (listen(socket_, backlog) == SOCKET_ERROR) {
    SetError(error, "listen");
    Close();
    return false;
  }
  port_ = port;
  return true;
}

void LoopbackListener::Close() noexcept {
  CloseSocket(socket_);
  socket_ = INVALID_SOCKET;
  port_ = 0;
  if (winsock_started_) {
    WSACleanup();
    winsock_started_ = false;
  }
}

LoopbackListener::AcceptStatus LoopbackListener::Accept(
    SOCKET* accepted_socket, int timeout_ms, std::string* error) const {
  if (accepted_socket != nullptr) *accepted_socket = INVALID_SOCKET;
  if (!is_open() || accepted_socket == nullptr) {
    if (error != nullptr) *error = "listener is not open";
    return AcceptStatus::Error;
  }
  if (timeout_ms >= 0) {
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_, &read_set);
    const int selected = select(0, &read_set, nullptr, nullptr, &timeout);
    if (selected == 0) return AcceptStatus::Timeout;
    if (selected == SOCKET_ERROR) {
      SetError(error, "select");
      return AcceptStatus::Error;
    }
  }
  sockaddr_storage address{};
  int address_length = sizeof(address);
  const SOCKET accepted = accept(socket_, reinterpret_cast<sockaddr*>(&address),
                                 &address_length);
  if (accepted == INVALID_SOCKET) {
    SetError(error, "accept");
    return AcceptStatus::Error;
  }
  if (!IsLoopbackPeer(accepted)) {
    CloseSocket(accepted);
    if (error != nullptr) *error = "rejected non-loopback peer";
    return AcceptStatus::Error;
  }
  *accepted_socket = accepted;
  return AcceptStatus::Accepted;
}

}  // namespace pdb::adb
