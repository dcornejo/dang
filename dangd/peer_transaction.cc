// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction.h"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

namespace dangd {
namespace {

using Participant = PeerTransactionParticipant;

std::vector<std::size_t> PrepareOrder(
    const std::vector<Participant>& participants) {
  std::vector<std::size_t> order(participants.size());
  for (std::size_t index = 0; index < order.size(); ++index)
    order[index] = index;
  std::ranges::sort(order, {},
                    [&](std::size_t index) { return participants[index].id; });
  return order;
}

std::vector<std::size_t> ApplyOrder(
    const std::vector<Participant>& participants) {
  std::vector<std::size_t> order = PrepareOrder(participants);
  std::ranges::stable_sort(order, {}, [&](std::size_t index) {
    return participants[index].role == PeerTransactionRole::kPrimary ? 1 : 0;
  });
  return order;
}

std::optional<std::string> ValidateParticipants(
    const std::vector<Participant>& participants, bool require_all_callbacks) {
  if (participants.size() < 2)
    return "peer transaction requires at least two participants";
  std::set<std::string> ids;
  std::size_t primary_count = 0;
  for (const Participant& participant : participants) {
    if (participant.id.empty() || !ids.insert(participant.id).second)
      return "peer transaction participant identifiers must be nonempty and "
             "unique";
    if (participant.role == PeerTransactionRole::kPrimary) ++primary_count;
    if (!participant.confirm || !participant.release)
      return participant.id +
             ": peer transaction recovery callbacks are incomplete";
    if (require_all_callbacks &&
        (!participant.prepare || !participant.apply_confirmed ||
         !participant.verify || !participant.cancel)) {
      return participant.id + ": peer transaction callbacks are incomplete";
    }
  }
  if (primary_count != 1)
    return "peer transaction requires exactly one primary participant";
  return std::nullopt;
}

std::optional<std::string> ValidateAbortParticipants(
    const std::vector<Participant>& participants) {
  if (participants.size() < 2)
    return "peer transaction requires at least two participants";
  std::set<std::string> ids;
  std::size_t primary_count = 0;
  for (const Participant& participant : participants) {
    if (participant.id.empty() || !ids.insert(participant.id).second)
      return "peer transaction participant identifiers must be nonempty and "
             "unique";
    if (participant.role == PeerTransactionRole::kPrimary) ++primary_count;
    if (!participant.cancel || !participant.release)
      return participant.id +
             ": peer transaction cancellation callback is incomplete";
  }
  if (primary_count != 1)
    return "peer transaction requires exactly one primary participant";
  return std::nullopt;
}

void ReleasePrepared(const std::vector<Participant>& participants,
                     const std::vector<std::size_t>& prepared) {
  for (auto index = prepared.rbegin(); index != prepared.rend(); ++index)
    participants[*index].release();
}

void CancelApplied(const std::vector<Participant>& participants,
                   const std::vector<std::size_t>& applied,
                   PeerTransactionResult* result) {
  for (auto index = applied.rbegin(); index != applied.rend(); ++index) {
    if (const auto error = participants[*index].cancel()) {
      result->rollback_failures.push_back(participants[*index].id + ": " +
                                          *error);
    }
  }
  if (!result->rollback_failures.empty())
    result->disposition = PeerTransactionDisposition::kRollbackIncomplete;
}

void CompleteAbort(const PeerTransactionJournal& journal,
                   PeerTransactionResult* result) {
  if (!result->rollback_failures.empty()) return;
  if (!journal.record_abort) {
    result->disposition = PeerTransactionDisposition::kRollbackIncomplete;
    result->message += result->message.empty() ? "" : "; ";
    result->message += "peer prepared journal abort callback is missing";
    return;
  }
  if (const auto error = journal.record_abort()) {
    result->disposition = PeerTransactionDisposition::kRollbackIncomplete;
    result->message += result->message.empty() ? "" : "; ";
    result->message += "cannot remove peer prepared journal: " + *error;
  }
}

std::vector<std::string> ParticipantIds(
    const std::vector<Participant>& participants,
    const std::vector<std::size_t>& order) {
  std::vector<std::string> ids;
  ids.reserve(order.size());
  for (const std::size_t index : order) ids.push_back(participants[index].id);
  return ids;
}

}  // namespace

PeerTransactionPrepareResult PeerTransactionCoordinator::Prepare(
    std::vector<PeerTransactionParticipant> participants,
    PeerTransactionJournal journal) const {
  PeerTransactionResult result;
  if (const auto error = ValidateParticipants(participants, true)) {
    result.message = *error;
    CompleteAbort(journal, &result);
    return {.result = std::move(result)};
  }
  if (!journal.record_abort || !journal.record_commit_decision ||
      !journal.record_confirmation || !journal.record_complete) {
    result.message = "peer transaction journal callbacks are incomplete";
    CompleteAbort(journal, &result);
    return {.result = std::move(result)};
  }

  const std::vector<std::size_t> prepare_order = PrepareOrder(participants);
  const std::vector<std::size_t> apply_order = ApplyOrder(participants);
  std::vector<std::size_t> prepared;
  std::vector<std::size_t> applied;

  for (const std::size_t index : prepare_order) {
    if (DeadlineExpired()) {
      result.message = "peer transaction deadline expired before preparing " +
                       participants[index].id;
      ReleasePrepared(participants, prepared);
      CompleteAbort(journal, &result);
      return {.result = std::move(result)};
    }
    // Preparation can fail after acquiring a remote lock. Include the
    // attempted participant in release cleanup even when its reply is an
    // error; release is required to be idempotent.
    prepared.push_back(index);
    if (const auto error = participants[index].prepare()) {
      result.message = participants[index].id + ": prepare failed: " + *error;
      ReleasePrepared(participants, prepared);
      CompleteAbort(journal, &result);
      return {.result = std::move(result)};
    }
    result.prepared.push_back(participants[index].id);
    if (DeadlineExpired()) {
      result.message = "peer transaction deadline expired while preparing " +
                       participants[index].id;
      ReleasePrepared(participants, prepared);
      CompleteAbort(journal, &result);
      return {.result = std::move(result)};
    }
  }

  for (const std::size_t index : apply_order) {
    if (DeadlineExpired()) {
      result.message =
          "peer transaction deadline expired before confirmed apply to " +
          participants[index].id;
      CancelApplied(participants, applied, &result);
      ReleasePrepared(participants, prepared);
      CompleteAbort(journal, &result);
      return {.result = std::move(result)};
    }
    // An error can be a lost reply after the confirmed commit took effect.
    // Cancel the attempted peer as well as earlier peers while the durable
    // group decision is still abort.
    applied.push_back(index);
    if (const auto error = participants[index].apply_confirmed()) {
      result.message =
          participants[index].id + ": confirmed apply failed: " + *error;
      CancelApplied(participants, applied, &result);
      ReleasePrepared(participants, prepared);
      CompleteAbort(journal, &result);
      return {.result = std::move(result)};
    }
    result.applied.push_back(participants[index].id);
    if (DeadlineExpired()) {
      result.message =
          "peer transaction deadline expired during confirmed apply to " +
          participants[index].id;
      CancelApplied(participants, applied, &result);
      ReleasePrepared(participants, prepared);
      CompleteAbort(journal, &result);
      return {.result = std::move(result)};
    }
  }

  for (const std::size_t index : apply_order) {
    if (DeadlineExpired()) {
      result.message =
          "peer transaction deadline expired before verification of " +
          participants[index].id;
      CancelApplied(participants, applied, &result);
      ReleasePrepared(participants, prepared);
      CompleteAbort(journal, &result);
      return {.result = std::move(result)};
    }
    if (const auto error = participants[index].verify()) {
      result.message = participants[index].id +
                       ": post-apply verification failed: " + *error;
      CancelApplied(participants, applied, &result);
      ReleasePrepared(participants, prepared);
      CompleteAbort(journal, &result);
      return {.result = std::move(result)};
    }
    if (DeadlineExpired()) {
      result.message =
          "peer transaction deadline expired during verification of " +
          participants[index].id;
      CancelApplied(participants, applied, &result);
      ReleasePrepared(participants, prepared);
      CompleteAbort(journal, &result);
      return {.result = std::move(result)};
    }
  }

  result.disposition = PeerTransactionDisposition::kPrepared;
  PeerPreparedTransaction transaction{.participants = std::move(participants),
                                      .journal = std::move(journal),
                                      .progress = result,
                                      .deadline_expired = deadline_expired_};
  return {.result = std::move(result), .transaction = std::move(transaction)};
}

PeerTransactionResult PeerTransactionCoordinator::CommitPrepared(
    PeerPreparedTransaction transaction,
    PeerDecisionFailurePolicy failure_policy) const {
  PeerTransactionResult result = std::move(transaction.progress);
  std::vector<PeerTransactionParticipant>& participants =
      transaction.participants;
  const std::vector<std::size_t> prepare_order = PrepareOrder(participants);
  const std::vector<std::size_t> apply_order = ApplyOrder(participants);
  const std::vector<std::string> participant_ids =
      ParticipantIds(participants, prepare_order);
  const auto deadline_expired = [&transaction] {
    return transaction.deadline_expired && transaction.deadline_expired();
  };

  if (deadline_expired()) {
    result.message =
        "peer transaction deadline expired before the commit decision";
    if (failure_policy == PeerDecisionFailurePolicy::kRetainPrepared) {
      result.disposition = PeerTransactionDisposition::kCommitPending;
      result.pending_confirmations = participant_ids;
      ReleasePrepared(participants, prepare_order);
      return result;
    }
    result.disposition = PeerTransactionDisposition::kAborted;
    CancelApplied(participants, apply_order, &result);
    ReleasePrepared(participants, prepare_order);
    CompleteAbort(transaction.journal, &result);
    return result;
  }

  const PeerTransactionDecisionResult decision =
      transaction.journal.record_commit_decision(participant_ids);
  if (decision.status == PeerTransactionDecisionStatus::kNotCommitted) {
    if (failure_policy == PeerDecisionFailurePolicy::kRetainPrepared) {
      result.disposition = PeerTransactionDisposition::kCommitPending;
      result.message =
          "cannot durably record peer commit decision; PREPARED recovery is "
          "required: " +
          decision.error;
      result.pending_confirmations = participant_ids;
      ReleasePrepared(participants, prepare_order);
      return result;
    }
    result.disposition = PeerTransactionDisposition::kAborted;
    result.message =
        "cannot durably record peer commit decision: " + decision.error;
    CancelApplied(participants, apply_order, &result);
    ReleasePrepared(participants, prepare_order);
    CompleteAbort(transaction.journal, &result);
    return result;
  }
  if (decision.status == PeerTransactionDecisionStatus::kOutcomeUnknown) {
    result.disposition = PeerTransactionDisposition::kCommitPending;
    result.decision_outcome_unknown = true;
    result.message =
        "peer commit decision outcome is unknown: " + decision.error;
    result.pending_confirmations = participant_ids;
    ReleasePrepared(participants, prepare_order);
    return result;
  }

  result.disposition = PeerTransactionDisposition::kCommitPending;
  for (std::size_t position = 0; position < apply_order.size(); ++position) {
    const std::size_t index = apply_order[position];
    if (deadline_expired()) {
      if (result.message.empty()) {
        result.message =
            "peer transaction deadline expired during commit confirmation";
      }
      for (; position < apply_order.size(); ++position) {
        result.pending_confirmations.push_back(
            participants[apply_order[position]].id);
      }
      break;
    }
    if (const auto error = participants[index].confirm()) {
      result.pending_confirmations.push_back(participants[index].id);
      if (result.message.empty()) {
        result.message = participants[index].id +
                         ": commit confirmation remains pending: " + *error;
      }
      continue;
    }
    if (const auto error =
            transaction.journal.record_confirmation(participants[index].id)) {
      result.pending_confirmations.push_back(participants[index].id);
      if (result.message.empty()) {
        result.message = participants[index].id +
                         ": confirmation succeeded but its acknowledgement "
                         "is not durable: " +
                         *error;
      }
      continue;
    }
    result.confirmed.push_back(participants[index].id);
  }
  ReleasePrepared(participants, prepare_order);

  if (!result.pending_confirmations.empty()) return result;
  if (const auto error = transaction.journal.record_complete()) {
    result.message =
        "peer commit completed but journal cleanup is pending: " + *error;
    result.journal_cleanup_pending = true;
    return result;
  }
  result.disposition = PeerTransactionDisposition::kCommitted;
  result.message.clear();
  return result;
}

PeerTransactionResult PeerTransactionCoordinator::AbortPrepared(
    PeerPreparedTransaction transaction) const {
  PeerTransactionResult result = std::move(transaction.progress);
  const std::vector<std::size_t> prepare_order =
      PrepareOrder(transaction.participants);
  const std::vector<std::size_t> apply_order =
      ApplyOrder(transaction.participants);
  result.disposition = PeerTransactionDisposition::kAborted;
  result.message.clear();
  CancelApplied(transaction.participants, apply_order, &result);
  ReleasePrepared(transaction.participants, prepare_order);
  CompleteAbort(transaction.journal, &result);
  return result;
}

PeerTransactionResult PeerTransactionCoordinator::Execute(
    std::vector<PeerTransactionParticipant> participants,
    PeerTransactionJournal journal) const {
  PeerTransactionPrepareResult prepared =
      Prepare(std::move(participants), std::move(journal));
  if (!prepared.ok()) {
    return std::move(prepared.result);
  }
  return CommitPrepared(std::move(*prepared.transaction));
}

PeerTransactionResult PeerTransactionCoordinator::ResumeCommit(
    std::vector<PeerTransactionParticipant> participants,
    std::vector<std::string> already_confirmed,
    PeerTransactionJournal journal) const {
  PeerTransactionResult result;
  result.disposition = PeerTransactionDisposition::kCommitPending;
  if (const auto error = ValidateParticipants(participants, false)) {
    result.message = *error;
    return result;
  }
  if (!journal.record_confirmation || !journal.record_complete) {
    result.message =
        "peer transaction recovery journal callbacks are incomplete";
    return result;
  }

  std::set<std::string> confirmed(already_confirmed.begin(),
                                  already_confirmed.end());
  if (confirmed.size() != already_confirmed.size()) {
    result.message =
        "peer transaction journal contains duplicate confirmations";
    return result;
  }
  std::set<std::string> participant_ids;
  for (const Participant& participant : participants)
    participant_ids.insert(participant.id);
  for (const std::string& id : confirmed) {
    if (!participant_ids.contains(id)) {
      result.message =
          "peer transaction journal contains unknown participant " + id;
      return result;
    }
  }
  result.confirmed = std::move(already_confirmed);

  for (const std::size_t index : ApplyOrder(participants)) {
    const std::string& id = participants[index].id;
    if (confirmed.contains(id)) continue;
    if (const auto error = participants[index].confirm()) {
      result.pending_confirmations.push_back(id);
      if (result.message.empty())
        result.message =
            id + ": commit confirmation remains pending: " + *error;
      continue;
    }
    if (const auto error = journal.record_confirmation(id)) {
      result.pending_confirmations.push_back(id);
      if (result.message.empty()) {
        result.message = id +
                         ": confirmation succeeded but its acknowledgement "
                         "is not durable: " +
                         *error;
      }
      continue;
    }
    result.confirmed.push_back(id);
  }
  for (const Participant& participant : participants) participant.release();

  if (!result.pending_confirmations.empty()) return result;
  if (const auto error = journal.record_complete()) {
    result.message =
        "peer commit completed but journal cleanup is pending: " + *error;
    result.journal_cleanup_pending = true;
    return result;
  }
  result.disposition = PeerTransactionDisposition::kCommitted;
  result.message.clear();
  return result;
}

PeerTransactionResult PeerTransactionCoordinator::ResumeAbort(
    std::vector<PeerTransactionParticipant> participants,
    PeerTransactionJournal journal) const {
  PeerTransactionResult result;
  if (const auto error = ValidateAbortParticipants(participants)) {
    result.disposition = PeerTransactionDisposition::kRollbackIncomplete;
    result.message = *error;
    return result;
  }
  if (!journal.record_abort) {
    result.disposition = PeerTransactionDisposition::kRollbackIncomplete;
    result.message = "peer transaction abort journal callback is incomplete";
    return result;
  }

  const std::vector<std::size_t> order = ApplyOrder(participants);
  for (auto index = order.rbegin(); index != order.rend(); ++index) {
    if (const auto error = participants[*index].cancel()) {
      result.rollback_failures.push_back(participants[*index].id + ": " +
                                         *error);
    }
  }
  for (const Participant& participant : participants) participant.release();
  if (!result.rollback_failures.empty()) {
    result.disposition = PeerTransactionDisposition::kRollbackIncomplete;
    result.message = "peer prepared transaction cancellation is incomplete";
    return result;
  }
  if (const auto error = journal.record_abort()) {
    result.disposition = PeerTransactionDisposition::kRollbackIncomplete;
    result.message = "cannot remove peer prepared journal: " + *error;
    return result;
  }
  result.disposition = PeerTransactionDisposition::kAborted;
  return result;
}

}  // namespace dangd
