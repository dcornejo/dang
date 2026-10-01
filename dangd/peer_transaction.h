// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PEER_TRANSACTION_H_
#define DANGD_PEER_TRANSACTION_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dangd {

/** Service role used to derive a continuity-preserving peer apply order. */
enum class PeerTransactionRole { kPrimary, kStandby };

/**
 * One independently managed participant in a distributed configuration change.
 *
 * The callbacks are transport-neutral. A NETCONF implementation can map them
 * to candidate lock/edit/validate, persistent confirmed-commit, health reads,
 * confirmation, cancellation, and candidate unlock/release operations
 * respectively.
 * Every callback must be idempotent because recovery may repeat it after a
 * lost reply. In particular, cancel must succeed harmlessly when an apply did
 * not take effect: the coordinator cancels an apply whose failure reply may be
 * ambiguous.
 */
struct PeerTransactionParticipant {
  /** Stable identity used for lock ordering and journal recovery. */
  std::string id;
  /** Current service role; exactly one participant must be primary. */
  PeerTransactionRole role = PeerTransactionRole::kStandby;
  /** Stages and validates the complete candidate without mutation. */
  std::function<std::optional<std::string>()> prepare;
  /** Starts a persistent, bounded confirmed commit. */
  std::function<std::optional<std::string>()> apply_confirmed;
  /** Verifies authoritative configuration and service health. */
  std::function<std::optional<std::string>()> verify;
  /** Makes this participant's confirmed commit permanent. */
  std::function<std::optional<std::string>()> confirm;
  /** Cancels an attempted confirmed commit before the group decision. */
  std::function<std::optional<std::string>()> cancel;
  /** Releases candidate, lock, and transport-session resources. */
  std::function<void()> release;
};

/** Durability classification for an attempted group COMMIT record. */
enum class PeerTransactionDecisionStatus {
  kNotCommitted,
  kCommitted,
  kOutcomeUnknown,
};

/** Result of crossing the durable group decision boundary. */
struct PeerTransactionDecisionResult {
  PeerTransactionDecisionStatus status =
      PeerTransactionDecisionStatus::kNotCommitted;
  std::string error;
};

/** Durable decision boundary supplied by the journal backend. */
struct PeerTransactionJournal {
  /** Makes the group commit decision crash-safe before returning success. */
  std::function<PeerTransactionDecisionResult(
      const std::vector<std::string> &participant_ids)>
      record_commit_decision;
  /** Durably records one acknowledged participant confirmation. */
  std::function<std::optional<std::string>(const std::string &participant_id)>
      record_confirmation;
  /** Marks recovery complete after every participant is confirmed. */
  std::function<std::optional<std::string>()> record_complete;
};

/** Externally meaningful outcome of a peer transaction attempt. */
enum class PeerTransactionDisposition {
  kAborted,
  kRollbackIncomplete,
  kCommitPending,
  kCommitted,
};

/** Result retained for operator reporting and durable recovery. */
struct PeerTransactionResult {
  /** Highest-confidence outcome reached by this coordinator attempt. */
  PeerTransactionDisposition disposition = PeerTransactionDisposition::kAborted;
  /** First actionable failure or pending-state explanation. */
  std::string message;
  /** Participants that completed preparation successfully. */
  std::vector<std::string> prepared;
  /** Participants that acknowledged successful confirmed apply. */
  std::vector<std::string> applied;
  /** Participants whose permanent confirmation was acknowledged. */
  std::vector<std::string> confirmed;
  /** Participants requiring idempotent confirmation recovery. */
  std::vector<std::string> pending_confirmations;
  /** Pre-decision cancellation failures requiring reconciliation. */
  std::vector<std::string> rollback_failures;
  /** True when only the journal's final completion record remains. */
  bool journal_cleanup_pending = false;
  /** True when storage cannot prove whether COMMIT crossed durability. */
  bool decision_outcome_unknown = false;

  /** Returns true only when all peers and journal state are complete. */
  [[nodiscard]] bool ok() const noexcept {
    return disposition == PeerTransactionDisposition::kCommitted;
  }
};

/**
 * Coordinates a fail-closed, confirmed-commit transaction across peers.
 *
 * Prepare is complete everywhere before mutation. Standbys are applied before
 * primaries, all participants are verified, and the commit decision is durably
 * recorded before any confirmation. Before that decision, failure cancels the
 * already applied peers in reverse order. After it, failure is a recoverable
 * pending confirmation and is never converted into rollback.
 */
class PeerTransactionCoordinator {
public:
  [[nodiscard]] PeerTransactionResult
  Execute(std::vector<PeerTransactionParticipant> participants,
          PeerTransactionJournal journal) const;

  /**
   * Replays an already durable commit decision after restart or a lost reply.
   *
   * `already_confirmed` comes from the journal. This method never invokes
   * prepare, apply, verify, or cancel. It releases the recovery sessions after
   * their confirmation attempts.
   */
  [[nodiscard]] PeerTransactionResult
  ResumeCommit(std::vector<PeerTransactionParticipant> participants,
               std::vector<std::string> already_confirmed,
               PeerTransactionJournal journal) const;
};

} // namespace dangd

#endif // DANGD_PEER_TRANSACTION_H_
