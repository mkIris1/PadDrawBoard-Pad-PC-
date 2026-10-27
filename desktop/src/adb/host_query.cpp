#include "pdb/adb/host_query.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pdb::adb {
namespace {

constexpr std::uint16_t kAdbServerPort = 5037;
// Loopback round trips finish in microseconds; these only guard against a
// wedged server so the poll can never block the session supervisor.
constexpr DWORD kSocketTimeoutMs = 400;
constexpr std::size_t kMaxPayloadBytes = 1u << 20;

// Releases the Winsock reference taken for one query.
struct WinsockScope {
  bool active{false};
  ~WinsockScope() {
    if (active) ::WSACleanup();
  }
};

bool SendAll(SOCKET socket, const char* data, int length) noexcept {
  int sent = 0;
  while (sent < length) {
    const int written = ::send(socket, data + sent, length - sent, 0);
    if (written <= 0) return false;
    sent += written;
  }
  return true;
}

bool ReadAll(SOCKET socket, char* data, int length) noexcept {
  int received = 0;
  while (received < length) {
    const int got = ::recv(socket, data + received, length - received, 0);
    if (got <= 0) return false;
    received += got;
  }
  return true;
}

bool ReadHexLength(SOCKET socket, std::size_t& length) noexcept {
  char text[4]{};
  if (!ReadAll(socket, text, 4)) return false;
  length = 0;
  for (const char ch : text) {
    int digit = 0;
    if (ch >= '0' && ch <= '9') {
      digit = ch - '0';
    } else if (ch >= 'a' && ch <= 'f') {
      digit = ch - 'a' + 10;
    } else if (ch >= 'A' && ch <= 'F') {
      digit = ch - 'A' + 10;
    } else {
      return false;
    }
    length = length * 16 + static_cast<std::size_t>(digit);
  }
  return true;
}

// The adb host protocol frames every request and reply as four lowercase hex
// digits giving the byte length, followed by the payload.
std::string FrameRequest(std::string_view service) {
  static constexpr char kHex[] = "0123456789abcdef";
  const std::size_t length = service.size();
  std::string framed;
  framed.reserve(length + 4);
  framed.push_back(kHex[(length >> 12) & 0xF]);
  framed.push_back(kHex[(length >> 8) & 0xF]);
  framed.push_back(kHex[(length >> 4) & 0xF]);
  framed.push_back(kHex[length & 0xF]);
  framed.append(service);
  return framed;
}

}  // namespace

std::optional<std::string> QueryAdbHostDevices(std::stop_token stop_token) {
  if (stop_token.stop_requested()) return std::nullopt;

  WinsockScope winsock;
  WSADATA data{};
  if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return std::nullopt;
  winsock.active = true;

  const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_SOCKET) return std::nullopt;

  const DWORD timeout = kSocketTimeoutMs;
  (void)::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  (void)::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(kAdbServerPort);
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  if (::connect(socket, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
    ::closesocket(socket);
    return std::nullopt;
  }

  std::optional<std::string> result;
  const std::string request = FrameRequest("host:devices-l");
  if (SendAll(socket, request.data(), static_cast<int>(request.size()))) {
    char status[4]{};
    if (ReadAll(socket, status, 4) && std::memcmp(status, "OKAY", 4) == 0) {
      std::size_t payload_length = 0;
      if (ReadHexLength(socket, payload_length) &&
          payload_length <= kMaxPayloadBytes) {
        std::string payload(payload_length, '\0');
        if (payload_length == 0 ||
            ReadAll(socket, payload.data(), static_cast<int>(payload_length))) {
          result = std::move(payload);
        }
      }
    }
  }
  ::closesocket(socket);
  return result;
}

}  // namespace pdb::adb
