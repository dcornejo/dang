// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/configuration_backend.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "yang/compiler.h"
#include "yang/module_resolver.h"
#include "yang/source_file.h"

namespace dangd {
namespace {

struct Fixture {
  yang::config::RuntimeSchema schema;
  yang::config::ConfigDocument before;
  yang::config::ConfigDocument after;
};

std::optional<Fixture> BuildFixture() {
  yang::VectorDiagnosticSink diagnostics;
  auto source = yang::SourceFile::Create("alpha.yang",
                                         R"yang(module alpha {
        yang-version 1.1; namespace "urn:test:alpha"; prefix a;
        container alpha { leaf value { type string; mandatory true; } }
      })yang",
                                         diagnostics);
  if (!source) return std::nullopt;
  yang::InMemoryModuleRepository repository;
  yang::Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation) return std::nullopt;
  auto schema =
      yang::config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto before = yang::config::ParseDatastoreXml(
      schema, "<alpha xmlns='urn:test:alpha'><value>old</value></alpha>");
  auto after = yang::config::ParseDatastoreXml(
      schema, "<alpha xmlns='urn:test:alpha'><value>new</value></alpha>");
  if (!before.document || !after.document) return std::nullopt;
  return Fixture{std::move(schema), std::move(*before.document),
                 std::move(*after.document)};
}

class FakePluginRuntime final : public PluginRuntime {
 public:
  explicit FakePluginRuntime(std::vector<std::string>* events)
      : events_(events) {}

  const std::vector<PluginYangSource>& yang_sources() const override {
    return sources_;
  }
  const std::vector<PluginManifest>& manifests() const override {
    return manifests_;
  }
  std::vector<PluginOperationalFragment> OperationalData() const override {
    return {};
  }
  std::vector<PluginPeerCandidate> PeerCandidates(
      std::optional<yang::config::ValidationFinding>*) override {
    std::vector<PluginPeerCandidate> result;
    for (const auto& [participant, role] :
         std::vector<std::pair<std::string, std::uint32_t>>{
             {"primary", DANG_PEER_PRIMARY_V1},
             {"standby", DANG_PEER_STANDBY_V1}}) {
      result.push_back(
          {.provider = "alpha-plugin",
           .group_id = "pair",
           .participant_id = participant,
           .role = role,
           .confirmed_timeout_seconds = 60,
           .module_name = "alpha",
           .configuration_xml =
               "<config xmlns='urn:ietf:params:xml:ns:netconf:base:1.0'>"
               "<alpha xmlns='urn:test:alpha'><value>new</value></alpha>"
               "</config>",
           .verification_context_json = "{}"});
    }
    return result;
  }
  std::optional<yang::config::ValidationFinding> VerifyPeer(
      const PluginPeerVerification& verification) override {
    events_->push_back("plugin verify " + verification.participant_id);
    if (fail_verify_) {
      yang::config::ValidationFinding finding;
      finding.code = yang::config::ValidationCode::kInvalidValue;
      finding.state = yang::config::FindingState::kInvalid;
      finding.message = "injected degraded peer health";
      finding.module_name = "alpha";
      finding.instance_path = "/alpha:alpha";
      finding.netconf_error_tag = "operation-failed";
      finding.netconf_error_app_tag = "peer-health-degraded";
      return finding;
    }
    return std::nullopt;
  }
  std::string ReconciliationData(
      std::span<const OperationalProviderFailure>) const override {
    return {};
  }
  std::optional<yang::config::ValidationFinding> Prepare(
      const yang::config::RuntimeSchema&, const yang::config::ConfigDocument&,
      const yang::config::ConfigDocument&,
      std::span<const yang::config::ChangeEvent>) override {
    events_->push_back("plugin prepare");
    return std::nullopt;
  }
  PluginApplyResult Apply(
      const yang::config::RuntimeSchema&,
      const yang::config::ConfigDocument& proposed) override {
    events_->push_back("plugin apply");
    if (fail_apply_) {
      yang::config::ValidationFinding finding;
      finding.message = "injected local apply failure";
      finding.netconf_error_tag = "operation-failed";
      return {std::move(finding), std::nullopt, {}};
    }
    return {std::nullopt, proposed, proposed.ToXml()};
  }
  void Abort() noexcept override { events_->push_back("plugin abort"); }
  yang::netconf::OperationResult InvokeRpc(
      const yang::netconf::RpcSessionContext&,
      const yang::config::RuntimeSchemaNode&, std::string_view) override {
    return {{false, {}, {}}, {}};
  }

  void set_fail_apply(bool fail) { fail_apply_ = fail; }
  void set_fail_verify(bool fail) { fail_verify_ = fail; }

 private:
  std::vector<std::string>* events_;
  bool fail_apply_ = false;
  bool fail_verify_ = false;
  std::vector<PluginYangSource> sources_;
  std::vector<PluginManifest> manifests_;
};

PeerParticipantFactory FakeParticipantFactory(
    std::vector<std::string>* events) {
  return [events](TlsPeerTransactionOptions options) {
    const std::string id = options.id;
    auto operation = [events, id](std::string name) {
      events->push_back(std::move(name) + " " + id);
      return std::optional<std::string>{};
    };
    return PeerTransactionParticipant{
        .id = options.id,
        .role = options.role,
        .prepare = [operation] { return operation("remote prepare"); },
        .apply_confirmed = [operation] { return operation("remote apply"); },
        .verify =
            [events, id, verify = std::move(options.verify_replies)] {
              events->push_back("remote verify " + id);
              const auto decision =
                  verify("<rpc-reply/>", "<rpc-reply/>");
              return decision.disposition ==
                             PeerVerificationDecision::Disposition::kAccepted
                  ? std::optional<std::string>{}
                  : std::optional<std::string>{decision.message};
            },
        .confirm = [operation] { return operation("remote confirm"); },
        .cancel = [operation] { return operation("remote cancel"); },
        .release = [events, id] { events->push_back("remote release " + id); }};
  };
}

std::vector<PeerRecoveryTarget> Targets() {
  return {{.group_id = "pair", .participant_id = "primary"},
          {.group_id = "pair", .participant_id = "standby"}};
}

TEST(EnglishConfigurationBackendTest,
     RetainsPreparedPeersUntilPostPersistenceFinalization) {
  auto fixture = BuildFixture();
  ASSERT_TRUE(fixture.has_value());
  std::vector<std::string> events;
  FakePluginRuntime plugins(&events);
  const auto journal = std::filesystem::temp_directory_path() /
                       "dangd-backend-peer-commit-test.json";
  std::error_code ignored;
  std::filesystem::remove(journal, ignored);
  unsigned token = 0;
  EnglishConfigurationBackend backend(
      fixture->before, &plugins, nullptr, false, Targets(), journal, true,
      FakeParticipantFactory(&events),
      [&token] { return "token-" + std::to_string(++token); });
  const auto changes = yang::config::DiffConfigDocuments(
      fixture->schema, fixture->before, fixture->after);

  ASSERT_FALSE(backend.PrepareReplacement(fixture->schema, fixture->before,
                                          fixture->after, changes));
  const auto recovery = backend.PreparedReplacementRecoveryState();
  ASSERT_TRUE(recovery.has_value());
  EXPECT_EQ(recovery->kind, "peer-transaction-v1");
  EXPECT_TRUE(std::filesystem::exists(journal));
  EXPECT_EQ(std::ranges::find_if(events,
                                 [](const std::string& event) {
                                   return event.starts_with("remote confirm");
                                 }),
            events.end());

  ASSERT_FALSE(backend.Replace(fixture->schema, fixture->before, fixture->after,
                               changes));
  ASSERT_FALSE(backend.CommitPreparedReplacement());
  EXPECT_FALSE(std::filesystem::exists(journal));
  EXPECT_NE(std::ranges::find(events, "remote confirm pair/standby"),
            events.end());
  EXPECT_NE(backend.WorkingXml().find("new"), std::string::npos);
  const std::string operational = backend.PeerTransactionOperationalXml();
  EXPECT_NE(operational.find("<phase>committed</phase>"), std::string::npos)
      << operational;
  EXPECT_NE(operational.find("<attempts>1</attempts>"), std::string::npos)
      << operational;
  EXPECT_NE(operational.find("<committed>1</committed>"), std::string::npos)
      << operational;
  EXPECT_NE(operational.find("<identity>pair/primary</identity>"),
            std::string::npos)
      << operational;
  EXPECT_NE(operational.find("<progress>confirmed</progress>"),
            std::string::npos)
      << operational;
  EXPECT_EQ(operational.find("token-"), std::string::npos) << operational;
}

TEST(EnglishConfigurationBackendTest,
     LocalApplyFailureCancelsPreparedPeersWithoutSelectingCommit) {
  auto fixture = BuildFixture();
  ASSERT_TRUE(fixture.has_value());
  std::vector<std::string> events;
  FakePluginRuntime plugins(&events);
  plugins.set_fail_apply(true);
  const auto journal = std::filesystem::temp_directory_path() /
                       "dangd-backend-peer-abort-test.json";
  std::error_code ignored;
  std::filesystem::remove(journal, ignored);
  unsigned token = 0;
  EnglishConfigurationBackend backend(
      fixture->before, &plugins, nullptr, false, Targets(), journal, true,
      FakeParticipantFactory(&events),
      [&token] { return "token-" + std::to_string(++token); });
  const auto changes = yang::config::DiffConfigDocuments(
      fixture->schema, fixture->before, fixture->after);

  ASSERT_FALSE(backend.PrepareReplacement(fixture->schema, fixture->before,
                                          fixture->after, changes));
  ASSERT_TRUE(backend.Replace(fixture->schema, fixture->before, fixture->after,
                              changes));
  backend.AbortPreparedReplacement();

  EXPECT_FALSE(std::filesystem::exists(journal));
  EXPECT_NE(std::ranges::find(events, "remote cancel pair/primary"),
            events.end());
  EXPECT_EQ(std::ranges::find_if(events,
                                 [](const std::string& event) {
                                   return event.starts_with("remote confirm");
                                 }),
            events.end());
  EXPECT_NE(backend.WorkingXml().find("old"), std::string::npos);
  const std::string operational = backend.PeerTransactionOperationalXml();
  EXPECT_NE(operational.find("<phase>aborted</phase>"), std::string::npos)
      << operational;
  EXPECT_NE(operational.find("<aborted>1</aborted>"), std::string::npos)
      << operational;
  EXPECT_NE(operational.find("<progress>applied</progress>"), std::string::npos)
      << operational;
}

TEST(EnglishConfigurationBackendTest,
     DegradedPeerVerificationCancelsRemoteWorkBeforeLocalApply) {
  auto fixture = BuildFixture();
  ASSERT_TRUE(fixture.has_value());
  std::vector<std::string> events;
  FakePluginRuntime plugins(&events);
  plugins.set_fail_verify(true);
  const auto journal = std::filesystem::temp_directory_path() /
                       "dangd-backend-peer-degraded-test.json";
  std::error_code ignored;
  std::filesystem::remove(journal, ignored);
  unsigned token = 0;
  EnglishConfigurationBackend backend(
      fixture->before, &plugins, nullptr, false, Targets(), journal, true,
      FakeParticipantFactory(&events),
      [&token] { return "token-" + std::to_string(++token); });
  const auto changes = yang::config::DiffConfigDocuments(
      fixture->schema, fixture->before, fixture->after);

  const auto error = backend.PrepareReplacement(
      fixture->schema, fixture->before, fixture->after, changes);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->netconf_error_app_tag, "peer-prepare-failed");
  EXPECT_NE(error->message.find("injected degraded peer health"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(journal));
  EXPECT_EQ(std::ranges::find(events, "plugin apply"), events.end());
  EXPECT_NE(std::ranges::find(events, "remote cancel pair/standby"),
            events.end());
  EXPECT_NE(std::ranges::find(events, "remote cancel pair/primary"),
            events.end());
  EXPECT_EQ(std::ranges::find_if(events,
                                 [](const std::string& event) {
                                   return event.starts_with("remote confirm");
                                 }),
            events.end());
  EXPECT_NE(backend.WorkingXml().find("old"), std::string::npos);
  const std::string operational = backend.PeerTransactionOperationalXml();
  EXPECT_NE(operational.find("<phase>aborted</phase>"), std::string::npos)
      << operational;
  EXPECT_NE(operational.find("<aborted>1</aborted>"), std::string::npos)
      << operational;
}

TEST(EnglishConfigurationBackendTest,
     ReportsDisabledPeerCoordinationWithoutPrivateConfiguration) {
  auto fixture = BuildFixture();
  ASSERT_TRUE(fixture.has_value());
  EnglishConfigurationBackend backend(fixture->before, nullptr, nullptr, false);

  const std::string operational = backend.PeerTransactionOperationalXml();
  EXPECT_NE(operational.find("<coordination-enabled>false"), std::string::npos)
      << operational;
  EXPECT_NE(operational.find("<phase>disabled</phase>"), std::string::npos)
      << operational;
  EXPECT_NE(operational.find("<attempts>0</attempts>"), std::string::npos)
      << operational;
}

TEST(EnglishConfigurationBackendTest,
     RefusesLivePeerWorkWithoutDurableLocalState) {
  auto fixture = BuildFixture();
  ASSERT_TRUE(fixture.has_value());
  std::vector<std::string> events;
  FakePluginRuntime plugins(&events);
  const auto journal = std::filesystem::temp_directory_path() /
                       "dangd-backend-peer-no-state-test.json";
  std::error_code ignored;
  std::filesystem::remove(journal, ignored);
  EnglishConfigurationBackend backend(
      fixture->before, &plugins, nullptr, false, Targets(), journal, false,
      FakeParticipantFactory(&events), [] { return "token"; });
  const auto changes = yang::config::DiffConfigDocuments(
      fixture->schema, fixture->before, fixture->after);

  const auto error = backend.PrepareReplacement(
      fixture->schema, fixture->before, fixture->after, changes);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->netconf_error_app_tag, "peer-state-persistence-required");
  EXPECT_FALSE(std::filesystem::exists(journal));
  EXPECT_EQ(std::ranges::find_if(events,
                                 [](const std::string& event) {
                                   return event.starts_with("remote prepare");
                                 }),
            events.end());
}

TEST(EnglishConfigurationBackendTest,
     StartupHydrationNeverOriginatesPeerCoordination) {
  auto fixture = BuildFixture();
  ASSERT_TRUE(fixture.has_value());
  std::vector<std::string> events;
  FakePluginRuntime plugins(&events);
  const auto journal = std::filesystem::temp_directory_path() /
                       "dangd-backend-peer-startup-test.json";
  std::error_code ignored;
  std::filesystem::remove(journal, ignored);
  EnglishConfigurationBackend backend(
      fixture->before, &plugins, nullptr, false, Targets(), journal, true,
      FakeParticipantFactory(&events), [] { return "token"; });

  ASSERT_FALSE(backend.Initialize(fixture->schema, fixture->after));

  EXPECT_FALSE(std::filesystem::exists(journal));
  EXPECT_EQ(std::ranges::find_if(events,
                                 [](const std::string& event) {
                                   return event.starts_with("remote prepare");
                                 }),
            events.end());
  EXPECT_NE(std::ranges::find(events, "plugin apply"), events.end());
}

}  // namespace
}  // namespace dangd
