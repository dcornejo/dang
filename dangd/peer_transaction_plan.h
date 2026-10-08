// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PEER_TRANSACTION_PLAN_H_
#define DANGD_PEER_TRANSACTION_PLAN_H_

#include <optional>
#include <string>
#include <vector>

#include "dangd/plugin_manager.h"
#include "yang/config_validation.h"

namespace dangd {

/** One plugin verifier retained for one composed peer participant. */
struct PeerPlanVerifier {
  std::string provider;
  std::string verification_context_json;
};

/** Complete candidate and verifier set for one stable peer identity. */
struct PeerPlanParticipant {
  std::string participant_id;
  bool local = false;
  std::uint32_t role = DANG_PEER_STANDBY_V1;
  std::uint32_t confirmed_timeout_seconds = 0;
  /** Namespaces whose complete module images replace remote running data. */
  std::vector<std::string> module_namespaces;
  std::string candidate_configuration;
  std::vector<PeerPlanVerifier> verifiers;
};

/** One independently coordinated peer group. */
struct ComposedPeerTransactionGroup {
  std::string group_id;
  std::vector<PeerPlanParticipant> participants;
};

/** Complete plan or an attributed fail-closed composition error. */
struct ComposePeerTransactionResult {
  std::vector<ComposedPeerTransactionGroup> groups;
  std::optional<yang::config::ValidationFinding> error;
};

/**
 * Composes non-overlapping complete module images into peer candidates.
 *
 * Every group must have at least two participants and exactly one primary.
 * Each module must be supplied once for every participant, role and timeout
 * must agree across contributors, and each fragment may contain only roots
 * owned by its declared module. The final candidate is schema validated only
 * after all module images have been combined, allowing modeled cross-module
 * dependencies without weakening completeness.
 */
[[nodiscard]] ComposePeerTransactionResult ComposePeerTransactionPlan(
    const yang::config::RuntimeSchema& schema,
    const std::vector<PluginPeerCandidate>& contributions);

}  // namespace dangd

#endif  // DANGD_PEER_TRANSACTION_PLAN_H_
