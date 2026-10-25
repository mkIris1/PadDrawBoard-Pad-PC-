#pragma once

#include "pdb/adb/adb_types.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <span>

namespace pdb::app {

enum class AuthChannel : std::uint8_t { Control = 1, Video = 2, Input = 3 };

inline constexpr std::array<std::uint8_t, 8> kAuthMagic{
    'P', 'D', 'B', 'A', 'U', 'T', 'H', '1'};
inline constexpr std::size_t kAuthPrefaceBytes =
    kAuthMagic.size() + 1 + adb::kSessionTokenBytes;

[[nodiscard]] inline std::array<std::uint8_t, kAuthPrefaceBytes>
BuildAuthPreface(const adb::SessionToken& token, AuthChannel channel) noexcept {
  std::array<std::uint8_t, kAuthPrefaceBytes> preface{};
  std::copy(kAuthMagic.begin(), kAuthMagic.end(), preface.begin());
  preface[kAuthMagic.size()] = static_cast<std::uint8_t>(channel);
  std::copy(token.begin(), token.end(), preface.begin() + kAuthMagic.size() + 1);
  return preface;
}

// The token comparison is constant-time for the complete 32-byte field. The
// magic and channel are checked separately because they are not secret.
[[nodiscard]] inline bool ValidateAuthPreface(
    std::span<const std::uint8_t> preface, const adb::SessionToken& expected,
    AuthChannel channel) noexcept {
  if (preface.size() != kAuthPrefaceBytes ||
      !std::equal(kAuthMagic.begin(), kAuthMagic.end(), preface.begin()) ||
      preface[kAuthMagic.size()] != static_cast<std::uint8_t>(channel)) {
    return false;
  }
  std::uint8_t difference = 0;
  const auto token_begin = preface.begin() + kAuthMagic.size() + 1;
  for (std::size_t index = 0; index < adb::kSessionTokenBytes; ++index) {
    difference = static_cast<std::uint8_t>(difference |
        (token_begin[index] ^ expected[index]));
  }
  return difference == 0;
}

}  // namespace pdb::app
