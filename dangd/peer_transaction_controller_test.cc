// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction_controller.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dangd {
namespace {

class PeerTransactionControllerTest : public testing::Test {
protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("dang-peer-controller-" + std::to_string(getpid()) + "-" +
                  std::to_string(++sequence_));
    ASSERT_TRUE(std::filesystem::create_directory(directory_));
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  static std::vector<PeerRecoveryTarget> Targets(bool include_standby = true) {
    std::vector<PeerRecoveryTarget> targets = {
        {.group_id = "test-group",
         .participant_id = "primary",
         .transport = {.host = "primary.example", .port = 6513}}};
    if (include_standby) {
      targets.push_back(
          {.group_id = "test-group",
           .participant_id = "standby",
           .transport = {.host = "standby.example", .port = 6513}});
    }
    return targets;
  }

  static ComposedPeerTransactionGroup Group() {
    return {.group_id = "test-group",
            .participants = {
                {.participant_id = "primary",
                 .role = DANG_PEER_PRIMARY_V1,
                 .confirmed_timeout_seconds = 90,
                 .module_namespaces = {"urn:test:primary"},
                 .candidate_configuration = "<config><primary/></config>",
                 .verifiers = {{"provider-a", "{\"member\":\"primary\"}"}}},
                {.participant_id = "standby",
                 .role = DANG_PEER_STANDBY_V1,
                 .confirmed_timeout_seconds = 90,
                 .module_namespaces = {"urn:test:standby"},
                 .candidate_configuration = "<config><standby/></config>",
                 .verifiers = {{"provider-a", "{\"member\":\"standby\"}"}}}}};
  }

  std::filesystem::path directory_;
  static inline unsigned sequence_ = 0;
};

struct FakeParticipantState {
  TlsPeerTransactionOptions options;
  std::vector<std::string> *events = nullptr;
};

PeerTransactionParticipant
FakeParticipant(TlsPeerTransactionOptions options,
                std::vector<std::string> *events,
                std::vector<TlsPeerTransactionOptions> *captured) {
  captured->push_back(options);
  auto state = std::make_shared<FakeParticipantState>(
      FakeParticipantState{std::move(options), events});
  const std::string id = state->options.id;
  const PeerTransactionRole role = state->options.role;
  const auto record = [state](std::string_view phase) {
    state->events->push_back(std::string(phase) + ":" + state->options.id);
  };
  return {.id = id,
          .role = role,
          .prepare =
              [record] {
                record("prepare");
                return std::optional<std::string>{};
              },
          .apply_confirmed =
              [record] {
                record("apply");
                return std::optional<std::string>{};
              },
          .verify =
              [state, record] {
                record("verify");
                const auto decision = state->options.verify_replies(
                    "<running/>", "<operational/>");
                return decision.disposition ==
                               PeerVerificationDecision::Disposition::kAccepted
                    ? std::optional<std::string>{}
                    : std::optional<std::string>{decision.message};
              },
          .confirm =
              [record] {
                record("confirm");
                return std::optional<std::string>{};
              },
          .cancel =
              [record] {
                record("cancel");
                return std::optional<std::string>{};
              },
          .release = [record] { record("release"); }};
}

TEST_F(PeerTransactionControllerTest,
       BindsTargetsTokensVerifiersAndJournalToOneGenericGroup) {
  std::vector<std::string> events;
  std::vector<TlsPeerTransactionOptions> captured;
  std::vector<PluginPeerVerification> verifications;
  unsigned token = 0;
  PeerTransactionController controller(
      Targets(),
      [&](const PluginPeerVerification &verification)
          -> std::optional<yang::config::ValidationFinding> {
        verifications.push_back(verification);
        return std::nullopt;
      },
      [&](TlsPeerTransactionOptions options) {
        return FakeParticipant(std::move(options), &events, &captured);
      },
      [&]() -> std::optional<std::string> {
        return "persistent-" + std::to_string(++token);
      });

  const auto journal_path = directory_ / "transaction.json";
  const PeerTransactionResult result = controller.Execute(
      Group(), journal_path, "transaction-1", "sha256:proposal");
  EXPECT_TRUE(result.ok()) << result.message;
  EXPECT_FALSE(std::filesystem::exists(journal_path));
  ASSERT_EQ(captured.size(), 2u);
  EXPECT_EQ(captured[0].id, "test-group/primary");
  EXPECT_EQ(captured[0].transport.host, "primary.example");
  EXPECT_EQ(captured[0].candidate_configuration, "<config><primary/></config>");
  EXPECT_EQ(captured[0].persistent_commit_id, "persistent-1");
  EXPECT_EQ(captured[1].persistent_commit_id, "persistent-2");
  ASSERT_TRUE(captured[0].deadline);
  ASSERT_TRUE(captured[1].deadline);
  EXPECT_EQ(captured[0].deadline, captured[1].deadline);
  ASSERT_EQ(verifications.size(), 2u);
  EXPECT_EQ(verifications[0].group_id, "test-group");
  EXPECT_EQ(verifications[0].participant_id, "standby");
  EXPECT_EQ(verifications[0].provider, "provider-a");
  EXPECT_EQ(verifications[0].running_reply_xml, "<running/>");
  EXPECT_EQ(verifications[0].operational_reply_xml, "<operational/>");
  EXPECT_EQ(events,
            (std::vector<std::string>{
                "prepare:test-group/primary", "prepare:test-group/standby",
                "apply:test-group/standby", "apply:test-group/primary",
                "verify:test-group/standby", "verify:test-group/primary",
                "confirm:test-group/standby", "confirm:test-group/primary",
                "release:test-group/standby", "release:test-group/primary"}));
}

TEST_F(PeerTransactionControllerTest,
       ExcludesLocalParticipantFromRemoteTransaction) {
  auto group = Group();
  group.participants.front().local = true;
  std::vector<std::string> events;
  std::vector<TlsPeerTransactionOptions> captured;
  unsigned token = 0;
  PeerTransactionController controller(
      {Targets().back()},
      [](const PluginPeerVerification &)
          -> std::optional<yang::config::ValidationFinding> {
        return std::nullopt;
      },
      [&](TlsPeerTransactionOptions options) {
        return FakeParticipant(std::move(options), &events, &captured);
      },
      [&]() -> std::optional<std::string> {
        return "persistent-" + std::to_string(++token);
      });

  const auto journal_path = directory_ / "remote-only.json";
  const PeerTransactionResult result = controller.Execute(
      group, journal_path, "transaction-local-primary", "sha256:proposal");

  EXPECT_TRUE(result.ok()) << result.message;
  EXPECT_FALSE(std::filesystem::exists(journal_path));
  ASSERT_EQ(captured.size(), 1u);
  EXPECT_EQ(captured.front().id, "test-group/standby");
  EXPECT_EQ(events,
            (std::vector<std::string>{"prepare:test-group/standby",
                                      "apply:test-group/standby",
                                      "verify:test-group/standby",
                                      "confirm:test-group/standby",
                                      "release:test-group/standby"}));
}

TEST_F(PeerTransactionControllerTest,
       ExposesPreparedJournalBoundaryBeforeCommitConfirmation) {
  std::vector<std::string> events;
  std::vector<TlsPeerTransactionOptions> captured;
  unsigned token = 0;
  PeerTransactionController controller(
      Targets(),
      [](const PluginPeerVerification &)
          -> std::optional<yang::config::ValidationFinding> {
        return std::nullopt;
      },
      [&](TlsPeerTransactionOptions options) {
        return FakeParticipant(std::move(options), &events, &captured);
      },
      [&]() -> std::optional<std::string> {
        return "persistent-" + std::to_string(++token);
      });

  const auto journal_path = directory_ / "staged.json";
  PeerTransactionControllerPrepareResult prepared = controller.Prepare(
      Group(), journal_path, "transaction-staged", "sha256:staged");
  ASSERT_TRUE(prepared.ok()) << prepared.result.message;
  EXPECT_EQ(prepared.result.disposition, PeerTransactionDisposition::kPrepared);
  EXPECT_EQ(prepared.prepared->transaction_id, "transaction-staged");
  EXPECT_EQ(prepared.prepared->proposal_digest, "sha256:staged");
  EXPECT_TRUE(std::filesystem::exists(journal_path));
  EXPECT_EQ(std::ranges::find(events, "confirm:test-group/standby"),
            events.end());

  std::string load_error;
  auto journal = PeerTransactionFileJournal::Load(journal_path, &load_error);
  ASSERT_TRUE(journal) << load_error;
  EXPECT_EQ(journal->state().decision, PeerJournalDecision::kPrepared);
  journal.reset();

  const PeerTransactionResult committed =
      controller.Commit(std::move(prepared.prepared));
  EXPECT_TRUE(committed.ok()) << committed.message;
  EXPECT_FALSE(std::filesystem::exists(journal_path));
  EXPECT_NE(std::ranges::find(events, "confirm:test-group/standby"),
            events.end());
}

TEST_F(PeerTransactionControllerTest,
       VerifierFailureCancelsEveryAppliedPeerBeforeDecision) {
  std::vector<std::string> events;
  std::vector<TlsPeerTransactionOptions> captured;
  unsigned token = 0;
  PeerTransactionController controller(
      Targets(),
      [](const PluginPeerVerification &verification)
          -> std::optional<yang::config::ValidationFinding> {
        if (verification.participant_id != "primary")
          return std::nullopt;
        yang::config::ValidationFinding finding;
        finding.message = "service is not healthy";
        finding.instance_path = "/health";
        return finding;
      },
      [&](TlsPeerTransactionOptions options) {
        return FakeParticipant(std::move(options), &events, &captured);
      },
      [&]() -> std::optional<std::string> {
        return "persistent-" + std::to_string(++token);
      });

  const auto journal_path = directory_ / "transaction.json";
  const PeerTransactionResult result = controller.Execute(
      Group(), journal_path, "transaction-2", "sha256:proposal");
  EXPECT_EQ(result.disposition, PeerTransactionDisposition::kAborted);
  EXPECT_NE(result.message.find("plugin provider-a rejected"),
            std::string::npos);
  EXPECT_NE(result.message.find("at /health"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(journal_path));
  EXPECT_NE(std::ranges::find(events, "cancel:test-group/primary"),
            events.end());
  EXPECT_NE(std::ranges::find(events, "cancel:test-group/standby"),
            events.end());
}

TEST_F(PeerTransactionControllerTest,
       DefaultPersistentTokensArePrivateBoundedAndUnique) {
  std::vector<std::string> events;
  std::vector<TlsPeerTransactionOptions> captured;
  PeerTransactionController controller(
      Targets(),
      [](const PluginPeerVerification &)
          -> std::optional<yang::config::ValidationFinding> {
        return std::nullopt;
      },
      [&](TlsPeerTransactionOptions options) {
        return FakeParticipant(std::move(options), &events, &captured);
      });
  const PeerTransactionResult result =
      controller.Execute(Group(), directory_ / "transaction.json",
                         "transaction-random", "sha256:proposal");
  ASSERT_TRUE(result.ok()) << result.message;
  ASSERT_EQ(captured.size(), 2u);
  EXPECT_EQ(captured[0].persistent_commit_id.size(), 70u);
  EXPECT_EQ(captured[0].persistent_commit_id.substr(0, 6), "dangd-");
  EXPECT_NE(captured[0].persistent_commit_id, captured[1].persistent_commit_id);
}

TEST_F(PeerTransactionControllerTest,
       RejectsMissingEndpointBeforeCreatingParticipantsOrJournal) {
  bool participant_created = false;
  PeerTransactionController controller(
      Targets(false), {},
      [&](TlsPeerTransactionOptions options) {
        participant_created = true;
        return MakeTlsTransactionParticipant(std::move(options));
      },
      [] { return std::optional<std::string>("persistent"); });
  const auto journal_path = directory_ / "transaction.json";
  const PeerTransactionResult result = controller.Execute(
      Group(), journal_path, "transaction-3", "sha256:proposal");
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.message.find("test-group/standby is not configured"),
            std::string::npos);
  EXPECT_FALSE(participant_created);
  EXPECT_FALSE(std::filesystem::exists(journal_path));
}

TEST_F(PeerTransactionControllerTest,
       RejectsDuplicatePersistentIdsBeforeNetworkMutation) {
  bool participant_prepared = false;
  PeerTransactionController controller(
      Targets(), {},
      [&](TlsPeerTransactionOptions options) {
        PeerTransactionParticipant participant =
            MakeTlsTransactionParticipant(std::move(options));
        participant.prepare = [&] {
          participant_prepared = true;
          return std::optional<std::string>{};
        };
        return participant;
      },
      [] { return std::optional<std::string>("duplicate"); });
  const auto journal_path = directory_ / "transaction.json";
  const PeerTransactionResult result = controller.Execute(
      Group(), journal_path, "transaction-4", "sha256:proposal");
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.message.find("unique persistent"), std::string::npos);
  EXPECT_FALSE(participant_prepared);
  EXPECT_FALSE(std::filesystem::exists(journal_path));
}

TEST_F(PeerTransactionControllerTest,
       RejectsTotalTimeoutThatCannotPrecedePeerRollback) {
  bool participant_created = false;
  PeerTransactionController controller(
      Targets(), {},
      [&](TlsPeerTransactionOptions options) {
        participant_created = true;
        return MakeTlsTransactionParticipant(std::move(options));
      },
      [] { return std::optional<std::string>("persistent"); },
      std::chrono::seconds(90));
  const auto journal_path = directory_ / "transaction.json";

  const PeerTransactionResult result = controller.Execute(
      Group(), journal_path, "transaction-timeout", "sha256:proposal");

  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.message.find("shorter than every"), std::string::npos);
  EXPECT_FALSE(participant_created);
  EXPECT_FALSE(std::filesystem::exists(journal_path));
}

} // namespace
} // namespace dangd
