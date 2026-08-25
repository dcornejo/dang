// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <optional>
#include <ranges>

#include <gtest/gtest.h>

#include "yang/compiler.h"
#include "yang/module_resolver.h"
#include "yang/netconf_datastore.h"
#include "yang/source_file.h"

namespace yang::netconf {
namespace {

struct FixtureData {
  config::RuntimeSchema schema;
  config::ConfigDocument initial;
};

class RecordingBackend final : public RunningConfigBackend {
 public:
  std::optional<config::ValidationFinding> Replace(
      const config::RuntimeSchema&,
      const config::ConfigDocument& before,
      const config::ConfigDocument& after,
      std::span<const config::ChangeEvent> observed) override {
    before_xml = before.ToXml();
    working_xml = after.ToXml();
    changes.assign(observed.begin(), observed.end());
    return std::nullopt;
  }

  std::string before_xml;
  std::string working_xml;
  std::vector<config::ChangeEvent> changes;
};

class RejectingBackend final : public RunningConfigBackend {
 public:
  std::optional<config::ValidationFinding> PrepareReplacement(
      const config::RuntimeSchema&, const config::ConfigDocument&,
      const config::ConfigDocument&,
      std::span<const config::ChangeEvent>) override {
    config::ValidationFinding finding;
    finding.code = config::ValidationCode::kInvalidValue;
    finding.state = config::FindingState::kInvalid;
    finding.message = "backend rejected the proposed configuration";
    finding.netconf_error_tag = "operation-failed";
    return finding;
  }
  std::optional<config::ValidationFinding> Replace(
      const config::RuntimeSchema&, const config::ConfigDocument&,
      const config::ConfigDocument&,
      std::span<const config::ChangeEvent>) override {
    applied = true;
    return std::nullopt;
  }
  void AbortPreparedReplacement() noexcept override { aborted = true; }
  bool applied = false;
  bool aborted = false;
};

std::optional<FixtureData> BuildFixture(VectorDiagnosticSink* diagnostics) {
  auto source = SourceFile::Create("store.yang", R"yang(module store {
    yang-version 1.1; namespace "urn:store"; prefix s;
    container system {
      leaf hostname { type string; mandatory true; }
      leaf enabled { type boolean; }
      leaf guarded { when "../enabled = 'true'"; type string; }
      choice transport {
        case tcp { leaf tcp-port { type uint16; } }
        case local { leaf socket { type string; } }
      }
    }
  })yang", *diagnostics);
  if (!source) return std::nullopt;
  InMemoryModuleRepository repository;
  Compiler compiler(repository, *diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation) return std::nullopt;
  config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto initial = config::ParseDatastoreXml(
      schema, R"xml(<system xmlns="urn:store"><hostname>old</hostname>
        <enabled>true</enabled><guarded>secret</guarded>
        <tcp-port>830</tcp-port></system>)xml")
                     .document;
  if (!initial) return std::nullopt;
  return FixtureData{std::move(schema), std::move(*initial)};
}

config::EditDocument HostnameEdit(const config::RuntimeSchema& schema,
                                  std::string_view value) {
  const std::string xml = "<system xmlns=\"urn:store\"><hostname>" +
                          std::string(value) + "</hostname></system>";
  return *config::ParseEditXml(schema, xml).document;
}

TEST(NetconfDatastoreTest, LocksEditsCandidateAndCommitsAtomically) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  EXPECT_TRUE(stores.Lock(Datastore::kCandidate, "one").ok);
  EXPECT_FALSE(stores.EditConfig(
      {"two", Datastore::kCandidate,
       {HostnameEdit(fixture->schema, "blocked")}}).ok);
  EXPECT_TRUE(stores.EditConfig(
      {"one", Datastore::kCandidate,
       {HostnameEdit(fixture->schema, "new")}}).ok);
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find(">new</"),
            std::string::npos);
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find(">old</"),
            std::string::npos);
  EXPECT_TRUE(stores.Commit("one").ok);
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find(">new</"),
            std::string::npos);
}

TEST(NetconfDatastoreTest, PersistenceFailureRollsBackLiveCommit) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  RecordingBackend backend;
  DatastoreManager stores(fixture->schema, fixture->initial, std::nullopt,
                          &backend);
  ASSERT_TRUE(stores.EditConfig(
      {"one", Datastore::kCandidate,
       {HostnameEdit(fixture->schema, "new")}}).ok);
  const std::string candidate_before =
      stores.Read(Datastore::kCandidate).ToXml();
  int attempts = 0;
  stores.SetPersistentStateCommitter(
      [&](const PersistentDatastoreState& before,
          const PersistentDatastoreState& after)
          -> std::optional<config::ValidationFinding> {
        ++attempts;
        EXPECT_NE(after.running_xml.find(">new</"), std::string::npos);
        EXPECT_NE(before.running_xml.find(">old</"), std::string::npos);
        config::ValidationFinding finding;
        finding.code = config::ValidationCode::kInvalidValue;
        finding.state = config::FindingState::kInvalid;
        finding.netconf_error_tag = "operation-failed";
        finding.message = "injected persistence failure";
        return finding;
      });

  const TransactionResult committed = stores.Commit("one");
  ASSERT_FALSE(committed.ok);
  ASSERT_EQ(committed.errors.size(), 1U);
  EXPECT_EQ(committed.errors.front().netconf_error_tag, "operation-failed");
  EXPECT_EQ(attempts, 1);
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find(">old</"),
            std::string::npos);
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml(), candidate_before);
  EXPECT_NE(backend.working_xml.find(">old</"), std::string::npos);
}

TEST(NetconfDatastoreTest, PublishesExactCommitChangesToRunningBackend) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  RecordingBackend backend;
  DatastoreManager stores(fixture->schema, fixture->initial, std::nullopt,
                          &backend);
  ASSERT_TRUE(stores.EditConfig(
      {"one", Datastore::kCandidate,
       {HostnameEdit(fixture->schema, "new")}}).ok);
  const TransactionResult committed = stores.Commit("one");
  ASSERT_TRUE(committed.ok);
  ASSERT_EQ(backend.changes.size(), 1u);
  EXPECT_EQ(backend.changes.front().kind,
            config::ChangeKind::kValueChanged);
  EXPECT_EQ(backend.changes.front().before, "old");
  EXPECT_EQ(backend.changes.front().after, "new");
  EXPECT_NE(backend.before_xml.find(">old</"), std::string::npos);
  EXPECT_NE(backend.working_xml.find(">new</"), std::string::npos);
}

TEST(NetconfDatastoreTest, BackendPreflightFailureDoesNotPublishOrArmRollback) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  RejectingBackend backend;
  DatastoreManager stores(fixture->schema, fixture->initial, std::nullopt,
                          &backend);
  ASSERT_TRUE(stores.EditConfig({
      .session = "one",
      .target = Datastore::kCandidate,
      .edits = {*config::ParseEditXml(fixture->schema, R"xml(
        <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
          <system xmlns="urn:store"><hostname>edge-2</hostname></system>
        </config>)xml").document}}).ok);
  ConfirmedCommitOptions confirmed;
  const TransactionResult result = stores.Commit("one", confirmed);
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(backend.aborted);
  EXPECT_FALSE(backend.applied);
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find("old"),
            std::string::npos);
  EXPECT_FALSE(stores.CancelCommit("one").ok);
}

TEST(NetconfDatastoreTest, SupportsTestOnlyDiscardAndRollbackOnError) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  EditConfigRequest test{"one", Datastore::kCandidate,
                         {HostnameEdit(fixture->schema, "tested")}};
  test.test_option = TestOption::kTestOnly;
  EXPECT_TRUE(stores.EditConfig(test).ok);
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml(),
            fixture->initial.ToXml());

  auto bad = config::ParseEditXml(fixture->schema, R"xml(
    <system xmlns="urn:store" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0">
      <hostname nc:operation="create">duplicate</hostname></system>)xml");
  ASSERT_TRUE(bad.document);
  EditConfigRequest rollback{"one", Datastore::kCandidate,
      {HostnameEdit(fixture->schema, "first"), *bad.document}};
  rollback.error_option = ErrorOption::kRollbackOnError;
  EXPECT_FALSE(stores.EditConfig(rollback).ok);
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml(),
            fixture->initial.ToXml());

  EXPECT_TRUE(stores.EditConfig(
      {"one", Datastore::kCandidate,
       {HostnameEdit(fixture->schema, "discard-me")}}).ok);
  EXPECT_TRUE(stores.DiscardChanges("one").ok);
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml(),
            stores.Read(Datastore::kRunning).ToXml());
}

TEST(NetconfDatastoreTest, SupportsDefaultOperationNone) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  auto edit = config::ParseEditXml(fixture->schema, R"xml(
    <system xmlns="urn:store"
            xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0">
      <hostname nc:operation="replace">new</hostname>
    </system>)xml");
  ASSERT_TRUE(edit.document);
  EditConfigRequest request{"one", Datastore::kCandidate, {*edit.document}};
  request.default_operation = config::EditOperation::kNone;
  EXPECT_TRUE(stores.EditConfig(request).ok);
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find(">new</"),
            std::string::npos);

  auto missing = config::ParseEditXml(fixture->schema, R"xml(
    <system xmlns="urn:store"
            xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
            nc:operation="delete"><hostname>old</hostname></system>)xml");
  ASSERT_TRUE(missing.document);
  EditConfigRequest delete_request{"one", Datastore::kCandidate,
                                   {*missing.document}};
  delete_request.default_operation = config::EditOperation::kNone;
  delete_request.test_option = TestOption::kSet;
  EXPECT_TRUE(stores.EditConfig(delete_request).ok);
  auto descendant = config::ParseEditXml(fixture->schema, R"xml(
    <system xmlns="urn:store"><hostname xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
      nc:operation="replace">x</hostname></system>)xml");
  ASSERT_TRUE(descendant.document);
  EditConfigRequest absent{"one", Datastore::kCandidate,
                           {*descendant.document}};
  absent.default_operation = config::EditOperation::kNone;
  absent.test_option = TestOption::kSet;
  EXPECT_FALSE(stores.EditConfig(absent).ok);
}

TEST(NetconfDatastoreTest, ConfirmsCancelsAndExpiresConfirmedCommits) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  ASSERT_TRUE(stores.EditConfig(
      {"one", Datastore::kCandidate,
       {HostnameEdit(fixture->schema, "temporary")}}).ok);
  ConfirmedCommitOptions options;
  options.timeout = std::chrono::seconds(60);
  options.persist = "token";
  EXPECT_TRUE(stores.Commit("one", options).ok);
  EXPECT_FALSE(stores.ConfirmCommit("other", "wrong").ok);
  EXPECT_TRUE(stores.CancelCommit("other", "token").ok);
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find(">old</"),
            std::string::npos);

  ASSERT_TRUE(stores.EditConfig(
      {"one", Datastore::kCandidate,
       {HostnameEdit(fixture->schema, "expires")}}).ok);
  EXPECT_TRUE(stores.Commit("one", options).ok);
  EXPECT_TRUE(stores.ProcessTimeouts(DatastoreManager::Clock::now() +
                                     std::chrono::hours(1)));
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find(">old</"),
            std::string::npos);
}

TEST(NetconfDatastoreTest, CopiesAndDeletesStartupConfiguration) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  EXPECT_TRUE(stores.CopyConfig("one", Datastore::kRunning,
                                Datastore::kStartup).ok);
  EXPECT_TRUE(stores.Validate(Datastore::kStartup).ok);
  EXPECT_TRUE(stores.DeleteConfig("one", Datastore::kStartup).ok);
  EXPECT_EQ(stores.Read(Datastore::kStartup).size(), 0U);
  EXPECT_FALSE(stores.DeleteConfig("one", Datastore::kRunning).ok);
}

TEST(NetconfDatastoreTest, AuthorizesExactCommitAndCopyChangesAtomically) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  ASSERT_TRUE(stores.EditConfig(
      {"one", Datastore::kCandidate,
       {HostnameEdit(fixture->schema, "protected")}}).ok);
  std::vector<config::ChangeEvent> observed;
  const auto deny_hostname = [&](const config::ChangeEvent& change) {
    observed.push_back(change);
    return change.instance_path.find("hostname") == std::string::npos;
  };
  TransactionResult commit =
      stores.Commit("one", std::nullopt, deny_hostname);
  EXPECT_FALSE(commit.ok);
  ASSERT_FALSE(commit.errors.empty());
  EXPECT_EQ(commit.errors.front().netconf_error_tag, "access-denied");
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find(">old</"),
            std::string::npos);
  ASSERT_FALSE(observed.empty());
  EXPECT_NE(observed.front().schema, config::kInvalidRuntimeSchemaNodeId);

  observed.clear();
  TransactionResult copied = stores.CopyConfig(
      "one", Datastore::kCandidate, Datastore::kStartup, deny_hostname);
  EXPECT_FALSE(copied.ok);
  EXPECT_NE(stores.Read(Datastore::kStartup).ToXml().find(">old</"),
            std::string::npos);
  EXPECT_FALSE(observed.empty());
}

TEST(NetconfDatastoreTest, DoesNotAuthorizeImplicitChoiceSideEffect) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  auto edit = config::ParseEditXml(fixture->schema, R"xml(
    <system xmlns="urn:store"><socket>/run/store.sock</socket></system>)xml");
  ASSERT_TRUE(edit.document);
  std::vector<std::string> authorized_paths;
  EditConfigRequest request;
  request.session = "one";
  request.target = Datastore::kRunning;
  request.edits.push_back(std::move(*edit.document));
  request.authorize_change = [&](const config::ChangeEvent& change) {
    authorized_paths.push_back(change.instance_path);
    return change.instance_path.find("tcp-port") == std::string::npos;
  };
  const TransactionResult changed = stores.EditConfig(request);
  EXPECT_TRUE(changed.ok) << (changed.errors.empty()
      ? "no error" : changed.errors.front().message);
  const std::string running = stores.Read(Datastore::kRunning).ToXml();
  EXPECT_NE(running.find("store.sock"), std::string::npos) << running;
  EXPECT_EQ(running.find("tcp-port"), std::string::npos) << running;
  EXPECT_TRUE(std::ranges::none_of(authorized_paths, [](const std::string& path) {
    return path.find("tcp-port") != std::string::npos;
  }));
}

TEST(NetconfDatastoreTest, DoesNotAuthorizeImplicitWhenSideEffect) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  auto edit = config::ParseEditXml(fixture->schema, R"xml(
    <system xmlns="urn:store"><enabled>false</enabled></system>)xml");
  ASSERT_TRUE(edit.document);
  std::vector<std::string> authorized_paths;
  EditConfigRequest request;
  request.session = "one";
  request.target = Datastore::kRunning;
  request.edits.push_back(std::move(*edit.document));
  request.authorize_change = [&](const config::ChangeEvent& change) {
    authorized_paths.push_back(change.instance_path);
    return change.instance_path.find("guarded") == std::string::npos;
  };
  const TransactionResult changed = stores.EditConfig(request);
  EXPECT_TRUE(changed.ok) << (changed.errors.empty()
      ? "no error" : changed.errors.front().message);
  const std::string running = stores.Read(Datastore::kRunning).ToXml();
  EXPECT_NE(running.find(">false</"), std::string::npos) << running;
  EXPECT_EQ(running.find("guarded"), std::string::npos) << running;
  EXPECT_TRUE(std::ranges::none_of(authorized_paths, [](const std::string& path) {
    return path.find("guarded") != std::string::npos;
  }));
}

TEST(NetconfDatastoreTest, SessionCloseReleasesLocksAndRollsBackConfirmation) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  ASSERT_TRUE(stores.Lock(Datastore::kCandidate, "one").ok);
  ASSERT_TRUE(stores.EditConfig(
      {"one", Datastore::kCandidate,
       {HostnameEdit(fixture->schema, "temporary")}}).ok);
  ConfirmedCommitOptions options;
  options.timeout = std::chrono::seconds(60);
  ASSERT_TRUE(stores.Commit("one", options).ok);
  stores.CloseSession("one");
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find(">old</"),
            std::string::npos);
  EXPECT_TRUE(stores.Lock(Datastore::kCandidate, "two").ok);
}

TEST(NetconfDatastoreTest, IntendedMirrorsRunningAndIsReadOnly) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture.has_value());
  DatastoreManager stores(fixture->schema, fixture->initial);
  EXPECT_EQ(stores.Read(Datastore::kIntended).ToXml(),
            stores.Read(Datastore::kRunning).ToXml());
  EXPECT_FALSE(stores.Lock(Datastore::kIntended, "one").ok);
  EXPECT_FALSE(stores.EditConfig(
      {"one", Datastore::kIntended,
       {HostnameEdit(fixture->schema, "new")}}).ok);
  EXPECT_FALSE(stores.CopyConfig("one", Datastore::kCandidate,
                                 Datastore::kIntended).ok);
  EXPECT_FALSE(stores.Lock(Datastore::kOperational, "one").ok);
  EXPECT_FALSE(stores.EditConfig(
      {"one", Datastore::kOperational,
       {HostnameEdit(fixture->schema, "new")}}).ok);
}

}  // namespace
}  // namespace yang::netconf
