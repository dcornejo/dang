// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PEER_TRANSACTION_TLS_H_
#define DANGD_PEER_TRANSACTION_TLS_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "dangd/peer_transaction.h"
#include "dangd/peer_transaction_journal.h"
#include "dangd/tls_transport.h"

namespace dangd {

/** Result of checking one authenticated peer readback. */
struct PeerVerificationDecision {
  /** Whether verification succeeded, needs fresh readback, or failed. */
  enum class Disposition { kAccepted, kPending, kRejected };
  Disposition disposition = Disposition::kRejected;
  std::string message;
};

/** Complete transport plan for one participant in a new peer transaction. */
struct TlsPeerTransactionOptions {
  /** Stable identity stored in the durable group journal. */
  std::string id;
  /** Service role used by the coordinator's continuity-preserving order. */
  PeerTransactionRole role = PeerTransactionRole::kStandby;
  /** Authenticated peer endpoint and credentials. */
  TlsClientOptions transport;
  /** Module namespaces authoritatively replaced by this transaction. */
  std::vector<std::string> module_namespaces;
  /** Complete images for those modules under a NETCONF `<config>` root. */
  std::string candidate_configuration;
  /** Unique RFC 6241 persistent confirmed-commit token. */
  std::string persistent_commit_id;
  /** Remote rollback deadline used by the confirmed commit. */
  std::uint32_t confirmed_timeout_seconds = 60;
  /** Shared forward-progress deadline for the complete peer transaction. */
  std::optional<std::chrono::steady_clock::time_point> deadline;
  /** Validates authenticated readback and may request bounded convergence. */
  std::function<PeerVerificationDecision(std::string_view, std::string_view)>
      verify_replies;
};

/** Builds all coordinator callbacks over one stateful mutual-TLS session. */
[[nodiscard]] PeerTransactionParticipant
MakeTlsTransactionParticipant(TlsPeerTransactionOptions options);

/** Confirms one RFC 6241 persistent commit over authenticated NETCONF/TLS. */
[[nodiscard]] std::optional<std::string>
ConfirmPersistentCommitOverTls(const TlsClientOptions &options,
                               const std::string &persistent_commit_id);

/** Idempotently cancels one persistent commit over authenticated NETCONF/TLS. */
[[nodiscard]] std::optional<std::string>
CancelPersistentCommitOverTls(const TlsClientOptions &options,
                              const std::string &persistent_commit_id);

/** Builds the confirmation/cancellation participant used by restart recovery. */
[[nodiscard]] PeerTransactionParticipant
MakeTlsRecoveryParticipant(const PeerJournalParticipant &journal_participant,
                           TlsClientOptions options);

} // namespace dangd

#endif // DANGD_PEER_TRANSACTION_TLS_H_
