// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PEER_RECOVERY_CONFIG_H_
#define DANGD_PEER_RECOVERY_CONFIG_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dangd/tls_transport.h"

namespace dangd {

/** Stable authenticated endpoint for one peer-group participant. */
struct PeerRecoveryTarget {
  /** Exact peer group identity supplied through the plugin contract. */
  std::string group_id;
  /** Exact participant identity within group_id. */
  std::string participant_id;
  /** Mutual-TLS endpoint and client credentials used during recovery. */
  TlsClientOptions transport;
};

/**
 * Returns the unambiguous host-owned identity used by coordination and journals.
 *
 * Both components have already been constrained by the public peer-plan
 * contract to exclude '/', making this encoding stable and reversible.
 */
[[nodiscard]] std::string PeerRecoveryTargetId(
    const PeerRecoveryTarget& target);

/**
 * Loads a private, bounded, versioned JSON peer-recovery configuration.
 *
 * Relative credential paths are resolved against the configuration file's
 * directory. The file must be a mode-0600 regular file owned by the effective
 * user and must not be a symbolic link.
 */
[[nodiscard]] std::optional<std::vector<PeerRecoveryTarget>>
LoadPeerRecoveryConfig(const std::filesystem::path& path, std::string* error);

}  // namespace dangd

#endif  // DANGD_PEER_RECOVERY_CONFIG_H_
