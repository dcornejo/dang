// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PEER_TRANSACTION_JOURNAL_H_
#define DANGD_PEER_TRANSACTION_JOURNAL_H_

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dangd/peer_transaction.h"

namespace dangd {

/** Recovery identity required to confirm one persistent remote commit. */
struct PeerJournalParticipant {
  /** Stable peer identity used by the coordinator. */
  std::string id;
  /** Service role captured with the durable decision. */
  PeerTransactionRole role = PeerTransactionRole::kStandby;
  /** RFC 6241 persistent confirmed-commit identifier for recovery. */
  std::string persistent_commit_id;
  /** True only after the confirmation acknowledgement is durable. */
  bool confirmed = false;
};

/** Durable group decision represented by the recovery journal. */
enum class PeerJournalDecision { kPrepared, kCommit };

/** Complete private state retained while a group transaction is unresolved. */
struct PeerJournalState {
  /** Stable operator-visible identity for the logical group transaction. */
  std::string transaction_id;
  /** Digest binding the journal to the exact logical proposal. */
  std::string proposal_digest;
  /** PREPARED before the group COMMIT decision, COMMIT afterwards. */
  PeerJournalDecision decision = PeerJournalDecision::kPrepared;
  /** Canonically ordered participant recovery records. */
  std::vector<PeerJournalParticipant> participants;
};

/** Atomic-save milestones exposed only for deterministic fault injection. */
enum class PeerJournalSaveStage {
  kTemporaryWritten,
  kTemporarySynchronized,
  kJournalReplaced,
  kDirectorySynchronized,
};

using PeerJournalSaveCheckpoint =
    std::function<bool(PeerJournalSaveStage stage)>;

/**
 * Private crash-safe JSON journal for one unresolved group commit.
 *
 * Creation durably records PREPARED before network mutation. The coordinator
 * atomically replaces that state with COMMIT before confirmation. A proven
 * pre-decision abort or final completion removes the journal and synchronizes
 * its parent directory.
 */
class PeerTransactionFileJournal {
public:
  /** Durably creates PREPARED and rejects an existing recovery file. */
  [[nodiscard]] static std::unique_ptr<PeerTransactionFileJournal>
  Create(const std::filesystem::path &path, PeerJournalState state,
         std::string *error,
         PeerJournalSaveCheckpoint checkpoint = PeerJournalSaveCheckpoint{});

  /** Loads and validates an existing private recovery journal. */
  [[nodiscard]] static std::unique_ptr<PeerTransactionFileJournal>
  Load(const std::filesystem::path &path, std::string *error,
       PeerJournalSaveCheckpoint checkpoint = PeerJournalSaveCheckpoint{});

  /** Returns coordinator callbacks backed by this journal's lifetime. */
  [[nodiscard]] PeerTransactionJournal Callbacks();

  /** Returns the last state successfully made durable. */
  [[nodiscard]] const PeerJournalState &state() const noexcept {
    return state_;
  }

private:
  PeerTransactionFileJournal(std::filesystem::path path, PeerJournalState state,
                             PeerJournalSaveCheckpoint checkpoint);

  [[nodiscard]] PeerTransactionDecisionResult
  RecordDecision(const std::vector<std::string> &participant_ids);
  [[nodiscard]] std::optional<std::string> RecordAbort();
  [[nodiscard]] std::optional<std::string>
  RecordConfirmation(const std::string &participant_id);
  [[nodiscard]] std::optional<std::string> RecordComplete();
  struct SaveResult {
    std::optional<std::string> error;
    bool replacement_may_be_visible = false;
  };
  [[nodiscard]] SaveResult Save();

  std::filesystem::path path_;
  PeerJournalState state_;
  PeerJournalSaveCheckpoint checkpoint_;
  bool decision_recorded_ = false;
};

} // namespace dangd

#endif // DANGD_PEER_TRANSACTION_JOURNAL_H_
