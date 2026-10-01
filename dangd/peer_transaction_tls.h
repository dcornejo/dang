// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PEER_TRANSACTION_TLS_H_
#define DANGD_PEER_TRANSACTION_TLS_H_

#include <optional>
#include <string>

#include "dangd/peer_transaction.h"
#include "dangd/peer_transaction_journal.h"
#include "dangd/tls_transport.h"

namespace dangd {

/** Confirms one RFC 6241 persistent commit over authenticated NETCONF/TLS. */
[[nodiscard]] std::optional<std::string> ConfirmPersistentCommitOverTls(
    const TlsClientOptions& options, const std::string& persistent_commit_id);

/** Builds the confirmation-only participant used by restart recovery. */
[[nodiscard]] PeerTransactionParticipant MakeTlsRecoveryParticipant(
    const PeerJournalParticipant& journal_participant,
    TlsClientOptions options);

}  // namespace dangd

#endif  // DANGD_PEER_TRANSACTION_TLS_H_
