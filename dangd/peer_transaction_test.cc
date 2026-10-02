// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace dangd {
namespace {

struct FakePeerState {
  std::vector<std::string> events;
  std::set<std::string> failures;
};

PeerTransactionParticipant Peer(std::string id, PeerTransactionRole role,
                                FakePeerState *state) {
  const std::string captured = id;
  auto operation = [state, captured](std::string name) {
    state->events.push_back(name + " " + captured);
    if (state->failures.contains(name + " " + captured))
      return std::optional<std::string>("injected " + name + " failure");
    return std::optional<std::string>{};
  };
  return {
      .id = std::move(id),
      .role = role,
      .prepare = [operation] { return operation("prepare"); },
      .apply_confirmed = [operation] { return operation("apply"); },
      .verify = [operation] { return operation("verify"); },
      .confirm = [operation] { return operation("confirm"); },
      .cancel = [operation] { return operation("cancel"); },
      .release = [state,
                  captured] { state->events.push_back("release " + captured); },
  };
}

PeerTransactionJournal Journal(FakePeerState *state, bool fail_decision = false,
                               bool fail_complete = false,
                               std::string fail_confirmation = {}) {
  return {
      .record_abort = [state] {
        state->events.push_back("abort journal");
        return std::optional<std::string>{};
      },
      .record_commit_decision =
          [state, fail_decision](const std::vector<std::string> &ids) {
            std::string event = "decision";
            for (const std::string &id : ids)
              event += " " + id;
            state->events.push_back(std::move(event));
            if (fail_decision)
              return PeerTransactionDecisionResult{
                  .status = PeerTransactionDecisionStatus::kNotCommitted,
                  .error = "journal unavailable"};
            return PeerTransactionDecisionResult{
                .status = PeerTransactionDecisionStatus::kCommitted};
          },
      .record_confirmation =
          [state, fail_confirmation](const std::string &id) {
            state->events.push_back("ack " + id);
            if (id == fail_confirmation)
              return std::optional<std::string>("journal unavailable");
            return std::optional<std::string>{};
          },
      .record_complete =
          [state, fail_complete] {
            state->events.push_back("complete");
            if (fail_complete)
              return std::optional<std::string>("journal cleanup failed");
            return std::optional<std::string>{};
          },
  };
}

std::vector<PeerTransactionParticipant> Pair(FakePeerState *state) {
  std::vector<PeerTransactionParticipant> peers;
  peers.push_back(Peer("primary", PeerTransactionRole::kPrimary, state));
  peers.push_back(Peer("standby", PeerTransactionRole::kStandby, state));
  return peers;
}

TEST(PeerTransactionCoordinatorTest,
     PreparesEveryPeerAndAppliesStandbyBeforePrimary) {
  FakePeerState state;
  const PeerTransactionResult result =
      PeerTransactionCoordinator().Execute(Pair(&state), Journal(&state));
  EXPECT_TRUE(result.ok()) << result.message;
  EXPECT_EQ(result.prepared, (std::vector<std::string>{"primary", "standby"}));
  EXPECT_EQ(result.applied, (std::vector<std::string>{"standby", "primary"}));
  EXPECT_EQ(result.confirmed, (std::vector<std::string>{"standby", "primary"}));
  EXPECT_EQ(state.events,
            (std::vector<std::string>{
                "prepare primary", "prepare standby", "apply standby",
                "apply primary", "verify standby", "verify primary",
                "decision primary standby", "confirm standby", "ack standby",
                "confirm primary", "ack primary", "release standby",
                "release primary", "complete"}));
}

TEST(PeerTransactionCoordinatorTest, PrepareFailurePreventsEveryMutation) {
  FakePeerState state;
  state.failures.insert("prepare standby");
  const PeerTransactionResult result =
      PeerTransactionCoordinator().Execute(Pair(&state), Journal(&state));
  EXPECT_EQ(result.disposition, PeerTransactionDisposition::kAborted);
  EXPECT_TRUE(result.applied.empty());
  EXPECT_EQ(state.events,
            (std::vector<std::string>{"prepare primary", "prepare standby",
                                      "release standby", "release primary",
                                      "abort journal"}));
}

TEST(PeerTransactionCoordinatorTest,
     ApplyFailureCancelsAttemptedAndPriorPeersInReverseOrder) {
  FakePeerState state;
  state.failures.insert("apply primary");
  const PeerTransactionResult result =
      PeerTransactionCoordinator().Execute(Pair(&state), Journal(&state));
  EXPECT_EQ(result.disposition, PeerTransactionDisposition::kAborted);
  EXPECT_EQ(result.applied, (std::vector<std::string>{"standby"}));
  EXPECT_LT(std::ranges::find(state.events, "cancel primary"),
            std::ranges::find(state.events, "cancel standby"));
  EXPECT_EQ(std::ranges::find(state.events, "decision primary standby"),
            state.events.end());
}

TEST(PeerTransactionCoordinatorTest,
     VerificationOrDecisionFailureCancelsBothPeers) {
  FakePeerState verify_state;
  verify_state.failures.insert("verify primary");
  const PeerTransactionResult verification =
      PeerTransactionCoordinator().Execute(Pair(&verify_state),
                                           Journal(&verify_state));
  EXPECT_EQ(verification.disposition, PeerTransactionDisposition::kAborted);
  EXPECT_LT(std::ranges::find(verify_state.events, "cancel primary"),
            std::ranges::find(verify_state.events, "cancel standby"));

  FakePeerState journal_state;
  const PeerTransactionResult journal = PeerTransactionCoordinator().Execute(
      Pair(&journal_state), Journal(&journal_state, true));
  EXPECT_EQ(journal.disposition, PeerTransactionDisposition::kAborted);
  EXPECT_LT(std::ranges::find(journal_state.events, "decision primary standby"),
            std::ranges::find(journal_state.events, "cancel primary"));
}

TEST(PeerTransactionCoordinatorTest,
     FailedCancellationIsReportedAsUnresolvedHardware) {
  FakePeerState state;
  state.failures.insert("apply primary");
  state.failures.insert("cancel standby");
  const PeerTransactionResult result =
      PeerTransactionCoordinator().Execute(Pair(&state), Journal(&state));
  EXPECT_EQ(result.disposition,
            PeerTransactionDisposition::kRollbackIncomplete);
  ASSERT_EQ(result.rollback_failures.size(), 1u);
  EXPECT_NE(result.rollback_failures.front().find("standby"),
            std::string::npos);
  EXPECT_EQ(std::ranges::find(state.events, "abort journal"),
            state.events.end());
}

TEST(PeerTransactionCoordinatorTest,
     DurableDecisionMakesLostConfirmationRecoverableWithoutRollback) {
  FakePeerState state;
  state.failures.insert("confirm standby");
  const PeerTransactionResult initial =
      PeerTransactionCoordinator().Execute(Pair(&state), Journal(&state));
  EXPECT_EQ(initial.disposition, PeerTransactionDisposition::kCommitPending);
  EXPECT_EQ(initial.confirmed, (std::vector<std::string>{"primary"}));
  EXPECT_EQ(initial.pending_confirmations,
            (std::vector<std::string>{"standby"}));
  EXPECT_EQ(std::ranges::find(state.events, "cancel standby"),
            state.events.end());

  state.failures.erase("confirm standby");
  state.events.clear();
  const PeerTransactionResult resumed =
      PeerTransactionCoordinator().ResumeCommit(Pair(&state), initial.confirmed,
                                                Journal(&state));
  EXPECT_TRUE(resumed.ok()) << resumed.message;
  EXPECT_EQ(std::ranges::find(state.events, "confirm primary"),
            state.events.end());
  EXPECT_NE(std::ranges::find(state.events, "confirm standby"),
            state.events.end());
  EXPECT_EQ(std::ranges::find(state.events, "cancel standby"),
            state.events.end());
}

TEST(PeerTransactionCoordinatorTest,
     CompletedPeersRemainCommitPendingUntilJournalCleanupSucceeds) {
  FakePeerState state;
  const PeerTransactionResult result = PeerTransactionCoordinator().Execute(
      Pair(&state), Journal(&state, false, true));
  EXPECT_EQ(result.disposition, PeerTransactionDisposition::kCommitPending);
  EXPECT_TRUE(result.pending_confirmations.empty());
  EXPECT_TRUE(result.journal_cleanup_pending);
}

TEST(PeerTransactionCoordinatorTest,
     ConfirmationRemainsPendingUntilAcknowledgementIsDurable) {
  FakePeerState state;
  const PeerTransactionResult result = PeerTransactionCoordinator().Execute(
      Pair(&state), Journal(&state, false, false, "standby"));
  EXPECT_EQ(result.disposition, PeerTransactionDisposition::kCommitPending);
  EXPECT_EQ(result.confirmed, (std::vector<std::string>{"primary"}));
  EXPECT_EQ(result.pending_confirmations,
            (std::vector<std::string>{"standby"}));
  EXPECT_EQ(std::ranges::find(state.events, "cancel standby"),
            state.events.end());
}

TEST(PeerTransactionCoordinatorTest,
     UnknownDecisionOutcomeNeverConfirmsOrRollsBack) {
  FakePeerState state;
  PeerTransactionJournal journal = Journal(&state);
  journal.record_commit_decision = [&state](const std::vector<std::string> &) {
    state.events.push_back("decision unknown");
    return PeerTransactionDecisionResult{
        .status = PeerTransactionDecisionStatus::kOutcomeUnknown,
        .error = "directory synchronization failed"};
  };
  const PeerTransactionResult result =
      PeerTransactionCoordinator().Execute(Pair(&state), std::move(journal));
  EXPECT_EQ(result.disposition, PeerTransactionDisposition::kCommitPending);
  EXPECT_TRUE(result.decision_outcome_unknown);
  EXPECT_EQ(result.pending_confirmations,
            (std::vector<std::string>{"primary", "standby"}));
  EXPECT_EQ(std::ranges::find(state.events, "confirm standby"),
            state.events.end());
  EXPECT_EQ(std::ranges::find(state.events, "cancel standby"),
            state.events.end());
}

TEST(PeerTransactionCoordinatorTest, RejectsInvalidPairIdentity) {
  FakePeerState state;
  std::vector<PeerTransactionParticipant> peers;
  peers.push_back(Peer("same", PeerTransactionRole::kPrimary, &state));
  peers.push_back(Peer("same", PeerTransactionRole::kStandby, &state));
  EXPECT_EQ(PeerTransactionCoordinator()
                .Execute(std::move(peers), Journal(&state))
                .disposition,
            PeerTransactionDisposition::kAborted);
  EXPECT_EQ(state.events, (std::vector<std::string>{"abort journal"}));
}

TEST(PeerTransactionCoordinatorTest,
     ResumeAbortCancelsEveryPossiblyAppliedPeerAndRemovesJournal) {
  FakePeerState state;
  const PeerTransactionResult result =
      PeerTransactionCoordinator().ResumeAbort(Pair(&state), Journal(&state));
  EXPECT_EQ(result.disposition, PeerTransactionDisposition::kAborted);
  EXPECT_TRUE(result.message.empty());
  EXPECT_EQ(state.events,
            (std::vector<std::string>{"cancel primary", "cancel standby",
                                      "release primary", "release standby",
                                      "abort journal"}));
}

TEST(PeerTransactionCoordinatorTest,
     ResumeAbortRetainsJournalWhenCancellationFails) {
  FakePeerState state;
  state.failures.insert("cancel primary");
  const PeerTransactionResult result =
      PeerTransactionCoordinator().ResumeAbort(Pair(&state), Journal(&state));
  EXPECT_EQ(result.disposition,
            PeerTransactionDisposition::kRollbackIncomplete);
  ASSERT_EQ(result.rollback_failures.size(), 1u);
  EXPECT_EQ(std::ranges::find(state.events, "abort journal"),
            state.events.end());
}

} // namespace
} // namespace dangd
