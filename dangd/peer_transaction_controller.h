// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PEER_TRANSACTION_CONTROLLER_H_
#define DANGD_PEER_TRANSACTION_CONTROLLER_H_

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dangd/peer_recovery_config.h"
#include "dangd/peer_transaction.h"
#include "dangd/peer_transaction_journal.h"
#include "dangd/peer_transaction_plan.h"
#include "dangd/peer_transaction_tls.h"

namespace dangd {

/** Routes authenticated peer readback through a retained plugin verifier. */
using PeerVerificationCallback =
    std::function<std::optional<yang::config::ValidationFinding>(
        const PluginPeerVerification&)>;

/** Creates one participant; injectable to test controller policy without I/O.
 */
using PeerParticipantFactory =
    std::function<PeerTransactionParticipant(TlsPeerTransactionOptions)>;

/** Creates a private persistent confirmed-commit identifier. */
using PeerPersistentIdFactory = std::function<std::optional<std::string>()>;

/** Owns the live sessions and journal for one verified PREPARED group. */
struct PreparedPeerTransactionHandle {
  std::unique_ptr<PeerTransactionFileJournal> journal;
  PeerPreparedTransaction transaction;
  std::string transaction_id;
  std::string proposal_digest;
};

/** Controller result at the pre-decision backend lifecycle boundary. */
struct PeerTransactionControllerPrepareResult {
  PeerTransactionResult result;
  std::unique_ptr<PreparedPeerTransactionHandle> prepared;

  [[nodiscard]] bool ok() const noexcept { return prepared != nullptr; }
};

/**
 * Materializes and coordinates one already composed peer transaction group.
 *
 * The controller owns endpoint lookup, persistent tokens, verifier routing,
 * durable PREPARED journal creation before network mutation, and coordinator
 * invocation. Its staged interface can retain verified PREPARED work across a
 * datastore durability boundary before choosing COMMIT. It deliberately
 * accepts no plugin-specific topology or transport policy.
 */
class PeerTransactionController {
 public:
  PeerTransactionController(std::vector<PeerRecoveryTarget> targets,
                            PeerVerificationCallback verify_peer,
                            PeerParticipantFactory participant_factory =
                                MakeTlsTransactionParticipant,
                            PeerPersistentIdFactory persistent_id_factory = {});

  /** Executes one group using the supplied stable transaction metadata. */
  [[nodiscard]] PeerTransactionResult Execute(
      const ComposedPeerTransactionGroup& group,
      const std::filesystem::path& journal_path, std::string transaction_id,
      std::string proposal_digest) const;

  /** Stops after verified confirmed applies while PREPARED is still durable. */
  [[nodiscard]] PeerTransactionControllerPrepareResult Prepare(
      const ComposedPeerTransactionGroup& group,
      const std::filesystem::path& journal_path, std::string transaction_id,
      std::string proposal_digest) const;

  /** Selects COMMIT and confirms a retained prepared group. */
  [[nodiscard]] PeerTransactionResult Commit(
      std::unique_ptr<PreparedPeerTransactionHandle> prepared,
      PeerDecisionFailurePolicy failure_policy =
          PeerDecisionFailurePolicy::kAbort) const;

  /** Cancels a retained prepared group without selecting COMMIT. */
  [[nodiscard]] PeerTransactionResult Abort(
      std::unique_ptr<PreparedPeerTransactionHandle> prepared) const;

 private:
  std::vector<PeerRecoveryTarget> targets_;
  PeerVerificationCallback verify_peer_;
  PeerParticipantFactory participant_factory_;
  PeerPersistentIdFactory persistent_id_factory_;
};

}  // namespace dangd

#endif  // DANGD_PEER_TRANSACTION_CONTROLLER_H_
