// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PEER_TRANSACTION_CONTROLLER_H_
#define DANGD_PEER_TRANSACTION_CONTROLLER_H_

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dangd/peer_recovery_config.h"
#include "dangd/peer_transaction.h"
#include "dangd/peer_transaction_plan.h"
#include "dangd/peer_transaction_tls.h"

namespace dangd {

/** Routes authenticated peer readback through a retained plugin verifier. */
using PeerVerificationCallback = std::function<
    std::optional<yang::config::ValidationFinding>(
        const PluginPeerVerification&)>;

/** Creates one participant; injectable to test controller policy without I/O. */
using PeerParticipantFactory =
    std::function<PeerTransactionParticipant(TlsPeerTransactionOptions)>;

/** Creates a private persistent confirmed-commit identifier. */
using PeerPersistentIdFactory = std::function<std::optional<std::string>()>;

/**
 * Materializes and executes one already composed peer transaction group.
 *
 * The controller owns endpoint lookup, persistent tokens, verifier routing,
 * journal creation, and coordinator invocation. It deliberately accepts no
 * plugin-specific topology or transport policy.
 */
class PeerTransactionController {
 public:
  PeerTransactionController(
      std::vector<PeerRecoveryTarget> targets,
      PeerVerificationCallback verify_peer,
      PeerParticipantFactory participant_factory =
          MakeTlsTransactionParticipant,
      PeerPersistentIdFactory persistent_id_factory = {});

  /** Executes one group using the supplied stable transaction metadata. */
  [[nodiscard]] PeerTransactionResult Execute(
      const ComposedPeerTransactionGroup& group,
      const std::filesystem::path& journal_path,
      std::string transaction_id, std::string proposal_digest) const;

 private:
  std::vector<PeerRecoveryTarget> targets_;
  PeerVerificationCallback verify_peer_;
  PeerParticipantFactory participant_factory_;
  PeerPersistentIdFactory persistent_id_factory_;
};

}  // namespace dangd

#endif  // DANGD_PEER_TRANSACTION_CONTROLLER_H_
