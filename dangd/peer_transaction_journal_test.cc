// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction_journal.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace dangd {
namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("dang-peer-journal-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

PeerJournalState State() {
  return {
      .transaction_id = "transaction-7",
      .proposal_digest = "sha256:0123456789abcdef",
      .participants = {{.id = "primary",
                        .role = PeerTransactionRole::kPrimary,
                        .persistent_commit_id = "persist-primary"},
                       {.id = "standby",
                        .role = PeerTransactionRole::kStandby,
                        .persistent_commit_id = "persist-standby"}},
  };
}

TEST(PeerTransactionFileJournalTest,
     PersistsDecisionAcknowledgementsAndCompletion) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "peer.json";
  std::string error;
  auto journal = PeerTransactionFileJournal::Create(path, State(), &error);
  ASSERT_TRUE(journal) << error;
  PeerTransactionJournal callbacks = journal->Callbacks();
  EXPECT_EQ(callbacks.record_commit_decision({"primary", "standby"}).status,
            PeerTransactionDecisionStatus::kCommitted);
#if defined(__unix__) || defined(__APPLE__)
  struct stat status{};
  ASSERT_EQ(stat(path.c_str(), &status), 0);
  EXPECT_EQ(status.st_mode & (S_IRWXG | S_IRWXO), 0u);
#endif

  auto loaded = PeerTransactionFileJournal::Load(path, &error);
  ASSERT_TRUE(loaded) << error;
  ASSERT_EQ(loaded->state().participants.size(), 2u);
  EXPECT_FALSE(loaded->state().participants[0].confirmed);
  EXPECT_FALSE(loaded->state().participants[1].confirmed);

  callbacks = loaded->Callbacks();
  EXPECT_FALSE(callbacks.record_confirmation("standby"));
  loaded = PeerTransactionFileJournal::Load(path, &error);
  ASSERT_TRUE(loaded) << error;
  EXPECT_FALSE(loaded->state().participants[0].confirmed);
  EXPECT_TRUE(loaded->state().participants[1].confirmed);

  callbacks = loaded->Callbacks();
  EXPECT_FALSE(callbacks.record_confirmation("primary"));
  EXPECT_FALSE(callbacks.record_complete());
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(PeerTransactionFileJournalTest, RejectsUnsafeOrInvalidRecoveryState) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "peer.json";
  std::string error;
  PeerJournalState duplicate = State();
  duplicate.participants[1].id = "primary";
  EXPECT_FALSE(PeerTransactionFileJournal::Create(path, duplicate, &error));

  std::ofstream(path) << "not-json\n";
#if defined(__unix__) || defined(__APPLE__)
  ASSERT_EQ(chmod(path.c_str(), S_IRUSR | S_IWUSR), 0);
#endif
  EXPECT_FALSE(PeerTransactionFileJournal::Load(path, &error));

#if defined(__unix__) || defined(__APPLE__)
  std::ofstream(path, std::ios::trunc) << "{}\n";
  ASSERT_EQ(chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP), 0);
  EXPECT_FALSE(PeerTransactionFileJournal::Load(path, &error));
#endif
}

TEST(PeerTransactionFileJournalTest,
     DistinguishesPreDecisionFailureFromUnknownDecisionOutcome) {
  const PeerJournalSaveStage stages[] = {
      PeerJournalSaveStage::kTemporaryWritten,
      PeerJournalSaveStage::kTemporarySynchronized,
      PeerJournalSaveStage::kJournalReplaced,
      PeerJournalSaveStage::kDirectorySynchronized};
  for (const PeerJournalSaveStage interrupted : stages) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "peer.json";
    std::string error;
    auto journal = PeerTransactionFileJournal::Create(
        path, State(), &error, [interrupted](PeerJournalSaveStage stage) {
          return stage != interrupted;
        });
    ASSERT_TRUE(journal) << error;
    const PeerTransactionDecisionResult result =
        journal->Callbacks().record_commit_decision({"primary", "standby"});
    if (interrupted == PeerJournalSaveStage::kTemporaryWritten ||
        interrupted == PeerJournalSaveStage::kTemporarySynchronized) {
      EXPECT_EQ(result.status, PeerTransactionDecisionStatus::kNotCommitted);
      EXPECT_FALSE(std::filesystem::exists(path));
    } else {
      EXPECT_EQ(result.status, PeerTransactionDecisionStatus::kOutcomeUnknown);
      auto recovered = PeerTransactionFileJournal::Load(path, &error);
      EXPECT_TRUE(recovered) << error;
    }
  }
}

TEST(PeerTransactionFileJournalTest, RefusesToReplaceUnresolvedJournal) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "peer.json";
  std::string error;
  auto first = PeerTransactionFileJournal::Create(path, State(), &error);
  ASSERT_TRUE(first) << error;
  ASSERT_EQ(
      first->Callbacks().record_commit_decision({"primary", "standby"}).status,
      PeerTransactionDecisionStatus::kCommitted);
  EXPECT_FALSE(PeerTransactionFileJournal::Create(path, State(), &error));
}

} // namespace
} // namespace dangd
