// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PEER_IDENTITY_H_
#define DANGD_PEER_IDENTITY_H_

#include <algorithm>
#include <string_view>

namespace dangd {

/** Returns whether one group or participant ID satisfies the public contract. */
[[nodiscard]] inline bool IsValidPeerIdentityComponent(
    std::string_view value) {
  if (value.empty() || value.size() > 128) return false;
  return std::ranges::all_of(value, [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
           byte == '-';
  });
}

}  // namespace dangd

#endif  // DANGD_PEER_IDENTITY_H_
