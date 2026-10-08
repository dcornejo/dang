// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction_controller.h"

#include <openssl/rand.h>

#include <array>
#include <map>
#include <set>
#include <utility>

#include "dangd/peer_identity.h"
#include "dangd/peer_transaction_journal.h"

namespace dangd {
namespace {

std::optional<std::string> SecurePersistentId() {
  std::array<unsigned char, 32> bytes{};
  if (RAND_priv_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    return std::nullopt;
  constexpr char kHex[] = "0123456789abcdef";
  std::string result = "dangd-";
  result.reserve(6 + bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

PeerTransactionRole Role(std::uint32_t role) {
  return role == DANG_PEER_PRIMARY_V1 ? PeerTransactionRole::kPrimary
                                      : PeerTransactionRole::kStandby;
}

}  // namespace

PeerTransactionController::PeerTransactionController(
    std::vector<PeerRecoveryTarget> targets,
    PeerVerificationCallback verify_peer,
    PeerParticipantFactory participant_factory,
    PeerPersistentIdFactory persistent_id_factory,
    std::chrono::milliseconds total_timeout)
    : targets_(std::move(targets)),
      verify_peer_(std::move(verify_peer)),
      participant_factory_(std::move(participant_factory)),
      persistent_id_factory_(std::move(persistent_id_factory)),
      total_timeout_(total_timeout) {
  if (!persistent_id_factory_) persistent_id_factory_ = SecurePersistentId;
}

PeerTransactionControllerPrepareResult PeerTransactionController::Prepare(
    const ComposedPeerTransactionGroup& group,
    const std::filesystem::path& journal_path, std::string transaction_id,
    std::string proposal_digest) const {
  PeerTransactionResult failed;
  if (!participant_factory_ || !persistent_id_factory_) {
    failed.message = "peer transaction controller callbacks are incomplete";
    return {.result = std::move(failed)};
  }
  if (total_timeout_ <= std::chrono::milliseconds::zero()) {
    failed.message = "peer transaction total timeout must be positive";
    return {.result = std::move(failed)};
  }
  for (const PeerPlanParticipant& participant : group.participants) {
    if (participant.local) continue;
    if (std::chrono::seconds(participant.confirmed_timeout_seconds) <=
        total_timeout_) {
      failed.message =
          "peer transaction total timeout must be shorter than every "
          "confirmed-commit rollback timeout";
      return {.result = std::move(failed)};
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + total_timeout_;

  std::map<std::string, const PeerRecoveryTarget*, std::less<>> targets;
  for (const PeerRecoveryTarget& target : targets_)
    targets.emplace(PeerRecoveryTargetId(target), &target);

  std::set<std::string> persistent_ids;
  struct MaterializedParticipant {
    const PeerPlanParticipant* planned = nullptr;
    const PeerRecoveryTarget* target = nullptr;
    std::string identity;
    std::string persistent_id;
  };
  std::vector<MaterializedParticipant> materialized;
  materialized.reserve(group.participants.size());
  PeerJournalState journal_state{.transaction_id = transaction_id,
                                 .proposal_digest = proposal_digest};
  journal_state.participants.reserve(group.participants.size());
  for (const PeerPlanParticipant& planned : group.participants) {
    if (planned.local) continue;
    const std::string identity =
        PeerIdentity(group.group_id, planned.participant_id);
    const auto target = targets.find(identity);
    if (target == targets.end()) {
      failed.message =
          "peer transaction target " + identity + " is not configured";
      return {.result = std::move(failed)};
    }
    const auto persistent_id = persistent_id_factory_();
    if (!persistent_id || persistent_id->empty() ||
        persistent_id->size() > 256 ||
        !persistent_ids.insert(*persistent_id).second) {
      failed.message = "cannot create a unique persistent peer commit identity";
      return {.result = std::move(failed)};
    }
    materialized.push_back({.planned = &planned,
                            .target = target->second,
                            .identity = identity,
                            .persistent_id = *persistent_id});
  }

  std::vector<PeerTransactionParticipant> participants;
  participants.reserve(materialized.size());
  for (const MaterializedParticipant& material : materialized) {
    const PeerPlanParticipant& planned = *material.planned;
    const PeerTransactionRole role = Role(planned.role);
    journal_state.participants.push_back(
        {.id = material.identity,
         .role = role,
         .persistent_commit_id = material.persistent_id});
    const auto verifiers = planned.verifiers;
    const PeerVerificationCallback verify_peer = verify_peer_;
    participants.push_back(participant_factory_({
        .id = material.identity,
        .role = role,
        .transport = material.target->transport,
        .module_namespaces = planned.module_namespaces,
        .candidate_configuration = planned.candidate_configuration,
        .persistent_commit_id = material.persistent_id,
        .confirmed_timeout_seconds = planned.confirmed_timeout_seconds,
        .deadline = deadline,
        .verify_replies =
            [verify_peer, verifiers, group_id = group.group_id,
             participant_id = planned.participant_id](
                std::string_view running,
                std::string_view operational) -> PeerVerificationDecision {
          if (!verify_peer && !verifiers.empty())
            return {PeerVerificationDecision::Disposition::kRejected,
                    "peer verifier callback is unavailable"};
          for (const PeerPlanVerifier& verifier : verifiers) {
            const auto finding = verify_peer(PluginPeerVerification{
                .provider = verifier.provider,
                .group_id = group_id,
                .participant_id = participant_id,
                .verification_context_json = verifier.verification_context_json,
                .running_reply_xml = std::string(running),
                .operational_reply_xml = std::string(operational)});
            if (finding) {
              std::string message =
                  "plugin " + verifier.provider +
                  " rejected peer verification: " + finding->message;
              if (!finding->instance_path.empty())
                message += " at " + finding->instance_path;
              return {
                  finding->netconf_error_app_tag ==
                          "peer-verification-pending"
                      ? PeerVerificationDecision::Disposition::kPending
                      : PeerVerificationDecision::Disposition::kRejected,
                  std::move(message)};
            }
          }
          return {PeerVerificationDecision::Disposition::kAccepted, {}};
        },
    }));
  }

  std::string journal_error;
  auto journal = PeerTransactionFileJournal::Create(
      journal_path, std::move(journal_state), &journal_error);
  if (!journal) {
    failed.message = "cannot create peer transaction journal: " + journal_error;
    return {.result = std::move(failed)};
  }
  PeerTransactionPrepareResult prepared =
      PeerTransactionCoordinator([deadline] {
        return std::chrono::steady_clock::now() >= deadline;
      }).Prepare(std::move(participants), journal->Callbacks());
  if (!prepared.ok()) return {.result = std::move(prepared.result)};
  auto handle = std::make_unique<PreparedPeerTransactionHandle>(
      PreparedPeerTransactionHandle{
          .journal = std::move(journal),
          .transaction = std::move(*prepared.transaction),
          .transaction_id = std::move(transaction_id),
          .proposal_digest = std::move(proposal_digest)});
  return {.result = std::move(prepared.result), .prepared = std::move(handle)};
}

PeerTransactionResult PeerTransactionController::Commit(
    std::unique_ptr<PreparedPeerTransactionHandle> prepared,
    PeerDecisionFailurePolicy failure_policy) const {
  if (!prepared) {
    PeerTransactionResult result;
    result.message = "prepared peer transaction handle is absent";
    return result;
  }
  return PeerTransactionCoordinator().CommitPrepared(
      std::move(prepared->transaction), failure_policy);
}

PeerTransactionResult PeerTransactionController::Abort(
    std::unique_ptr<PreparedPeerTransactionHandle> prepared) const {
  if (!prepared) {
    PeerTransactionResult result;
    result.message = "prepared peer transaction handle is absent";
    return result;
  }
  return PeerTransactionCoordinator().AbortPrepared(
      std::move(prepared->transaction));
}

PeerTransactionResult PeerTransactionController::Execute(
    const ComposedPeerTransactionGroup& group,
    const std::filesystem::path& journal_path, std::string transaction_id,
    std::string proposal_digest) const {
  PeerTransactionControllerPrepareResult prepared =
      Prepare(group, journal_path, std::move(transaction_id),
              std::move(proposal_digest));
  if (!prepared.ok()) return std::move(prepared.result);
  return Commit(std::move(prepared.prepared));
}

}  // namespace dangd
