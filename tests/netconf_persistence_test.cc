// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>

#include <gtest/gtest.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "yang/compiler.h"
#include "yang/module_resolver.h"
#include "yang/netconf_persistence.h"
#include "yang/source_file.h"

namespace yang::netconf {
namespace {

struct PersistenceFixture {
  config::RuntimeSchema schema;
  config::ConfigDocument initial;
};

std::optional<PersistenceFixture> BuildPersistenceFixture(
    VectorDiagnosticSink* diagnostics) {
  auto source = SourceFile::Create("persist.yang", R"yang(module persist {
    yang-version 1.1; namespace "urn:persist"; prefix p;
    leaf value { type string; mandatory true; }
  })yang", *diagnostics);
  if (!source) return std::nullopt;
  InMemoryModuleRepository repository;
  Compiler compiler(repository, *diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation) return std::nullopt;
  config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto initial = config::ParseDatastoreXml(
      schema, R"xml(<value xmlns="urn:persist">old</value>)xml").document;
  if (!initial) return std::nullopt;
  return PersistenceFixture{std::move(schema), std::move(*initial)};
}

config::EditDocument ValueEdit(const config::RuntimeSchema& schema,
                               std::string_view value) {
  return *config::ParseEditXml(
      schema, "<value xmlns=\"urn:persist\">" + std::string(value) +
                  "</value>").document;
}

TEST(NetconfPersistenceTest, SavesAndRestoresAllDatastores) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildPersistenceFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager source(fixture->schema, fixture->initial);
  ASSERT_TRUE(source.EditConfig(
      {"one", Datastore::kCandidate,
       {ValueEdit(fixture->schema, "candidate")}}).ok);
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "yang-netconf-snapshot-test.json";
  ASSERT_TRUE(SaveDatastoreSnapshot(path, source).ok);
  std::ifstream snapshot(path);
  const std::string serialized((std::istreambuf_iterator<char>(snapshot)), {});
  EXPECT_NE(serialized.find("\"rollback-running\": null"), std::string::npos);
  EXPECT_NE(serialized.find("\"confirmation-expiry-unix-seconds\": null"),
            std::string::npos);
  EXPECT_NE(serialized.find("\"confirming-session\": null"),
            std::string::npos);
  EXPECT_NE(serialized.find("\"persist-token\": null"), std::string::npos);
  EXPECT_NE(serialized.find("\"rollback-externally-coordinated\": false"),
            std::string::npos);
  EXPECT_NE(serialized.find("\"backend-recovery\": null"),
            std::string::npos);
  EXPECT_NE(serialized.find("\"version\": 2"), std::string::npos);

  DatastoreManager restored(fixture->schema, fixture->initial);
  ASSERT_TRUE(LoadDatastoreSnapshot(path, restored).ok);
  EXPECT_NE(restored.Read(Datastore::kCandidate).ToXml().find("candidate"),
            std::string::npos);
  EXPECT_NE(restored.Read(Datastore::kRunning).ToXml().find("old"),
            std::string::npos);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

TEST(NetconfPersistenceTest, RoundTripsOpaqueBackendRecoveryState) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildPersistenceFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager source(fixture->schema, fixture->initial);
  PersistentDatastoreState state = source.ExportPersistentState();
  state.backend_recovery = BackendRecoveryState{
      .kind = "peer-transaction-v1",
      .transaction_id = "tx-persisted",
      .proposal_digest = "sha256:digest"};
  ASSERT_TRUE(source.RestorePersistentState(
      state, DatastoreManager::RestoreBackend::kDefer).ok);
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
      "yang-netconf-backend-recovery-test.json";
  ASSERT_TRUE(SaveDatastoreSnapshot(path, source).ok);

  DatastoreManager restored(fixture->schema, fixture->initial);
  EXPECT_FALSE(LoadDatastoreSnapshot(path, restored).ok);
  ASSERT_TRUE(LoadDatastoreSnapshot(
      path, restored, DatastoreManager::RestoreBackend::kDefer).ok);
  ASSERT_TRUE(restored.ExportPersistentState().backend_recovery);
  EXPECT_EQ(*restored.ExportPersistentState().backend_recovery,
            *state.backend_recovery);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

TEST(NetconfPersistenceTest, RejectsMalformedBackendRecoveryState) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildPersistenceFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  EXPECT_FALSE(LoadDatastoreSnapshotJson(R"json({
    "version": 2,
    "running": "<value xmlns=\"urn:persist\">old</value>",
    "candidate": "<value xmlns=\"urn:persist\">old</value>",
    "startup": "<value xmlns=\"urn:persist\">old</value>",
    "backend-recovery": {
      "kind": "peer-transaction-v1",
      "transaction-id": "",
      "proposal-digest": "sha256:digest"
    }
  })json", stores, DatastoreManager::RestoreBackend::kDefer).ok);
}

TEST(NetconfPersistenceTest, RoundTripsExternalConfirmedCommitContext) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildPersistenceFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager source(fixture->schema, fixture->initial);
  ASSERT_TRUE(source.EditConfig(
      {"peer", Datastore::kCandidate,
       {ValueEdit(fixture->schema, "temporary")}}).ok);
  ConfirmedCommitOptions options;
  options.persist = "secret";
  const BackendTransactionContext external{
      .externally_coordinated = true};
  ASSERT_TRUE(source.Commit("peer", options, {}, external).ok);
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
      "yang-netconf-external-confirmed-test.json";
  ASSERT_TRUE(SaveDatastoreSnapshot(path, source).ok);

  DatastoreManager restored(fixture->schema, fixture->initial);
  ASSERT_TRUE(LoadDatastoreSnapshot(path, restored).ok);
  EXPECT_TRUE(
      restored.ExportPersistentState().rollback_externally_coordinated);
  EXPECT_FALSE(restored.CancelCommit("peer", "secret").ok);
  EXPECT_TRUE(restored.CancelCommit("peer", "secret", external).ok);
  EXPECT_NE(restored.Read(Datastore::kRunning).ToXml().find("old"),
            std::string::npos);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

TEST(NetconfPersistenceTest, ProtectsSnapshotAndRejectsUnsafeRestorePaths) {
#if defined(__unix__) || defined(__APPLE__)
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildPersistenceFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("yang-private-snapshot-" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch().count()));
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  const std::filesystem::path path = directory / "state.json";
  ASSERT_TRUE(SaveDatastoreSnapshot(path, stores).ok);
  struct stat status {};
  ASSERT_EQ(stat(path.c_str(), &status), 0);
  EXPECT_EQ(status.st_mode & (S_IRWXG | S_IRWXO), 0);

  ASSERT_EQ(chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP), 0);
  EXPECT_FALSE(LoadDatastoreSnapshot(path, stores).ok);
  ASSERT_EQ(chmod(path.c_str(), S_IRUSR | S_IWUSR), 0);
  const std::filesystem::path link = directory / "state-link.json";
  ASSERT_EQ(symlink(path.c_str(), link.c_str()), 0);
  EXPECT_FALSE(LoadDatastoreSnapshot(link, stores).ok);

  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
#else
  GTEST_SKIP() << "POSIX snapshot permissions are not available";
#endif
}

TEST(NetconfPersistenceTest, RestoresOrExpiresConfirmedCommitSafely) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildPersistenceFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  ASSERT_TRUE(stores.EditConfig(
      {"one", Datastore::kCandidate,
       {ValueEdit(fixture->schema, "temporary")}}).ok);
  ConfirmedCommitOptions options;
  options.persist = "token";
  ASSERT_TRUE(stores.Commit("one", options).ok);
  PersistentDatastoreState state = stores.ExportPersistentState();
  ASSERT_TRUE(state.rollback_running_xml);
  state.confirmation_expiry_unix_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count() - 1;
  DatastoreManager restored(fixture->schema, fixture->initial);
  ASSERT_TRUE(restored.RestorePersistentState(state).ok);
  EXPECT_NE(restored.Read(Datastore::kRunning).ToXml().find("old"),
            std::string::npos);
  EXPECT_FALSE(restored.ExportPersistentState().rollback_running_xml);
}

TEST(NetconfPersistenceTest, RejectsCorruptAndSchemaInvalidSnapshots) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildPersistenceFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "yang-netconf-corrupt-test.json";
  {
    std::ofstream output(path);
    output << "not-json";
  }
  EXPECT_FALSE(LoadDatastoreSnapshot(path, stores).ok);
  PersistentDatastoreState invalid = stores.ExportPersistentState();
  invalid.running_xml = "<unknown/>";
  EXPECT_FALSE(stores.RestorePersistentState(invalid).ok);
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find("old"),
            std::string::npos);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

TEST(NetconfPersistenceTest, RestoresSnapshotFromMemoryWithoutFilesystemIo) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildPersistenceFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  EXPECT_TRUE(LoadDatastoreSnapshotJson(R"json({
    "version": 1,
    "running": "<value xmlns=\"urn:persist\">memory</value>",
    "candidate": "<value xmlns=\"urn:persist\">memory</value>",
    "startup": "<value xmlns=\"urn:persist\">memory</value>"
  })json", stores).ok);
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find("memory"),
            std::string::npos);
  EXPECT_FALSE(LoadDatastoreSnapshotJson("not-json", stores).ok);
}

TEST(NetconfPersistenceTest, ReadsVersionOneSnapshotsWithoutRecoveryState) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildPersistenceFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  ASSERT_TRUE(LoadDatastoreSnapshotJson(R"json({
    "version": 1,
    "running": "<value xmlns=\"urn:persist\">legacy</value>",
    "candidate": "<value xmlns=\"urn:persist\">legacy</value>",
    "startup": "<value xmlns=\"urn:persist\">legacy</value>"
  })json", stores).ok);
  EXPECT_FALSE(stores.ExportPersistentState().backend_recovery);
}

TEST(NetconfPersistenceTest, AtomicSnapshotSurvivesEveryInterruptedSaveStage) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildPersistenceFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager old_stores(fixture->schema, fixture->initial);
  DatastoreManager new_stores(fixture->schema, fixture->initial);
  ASSERT_TRUE(new_stores.EditConfig(
      {"one", Datastore::kCandidate,
       {ValueEdit(fixture->schema, "new")}}).ok);

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("yang-snapshot-faults-" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch().count()));
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  const std::filesystem::path path = directory / "state.json";
  ASSERT_TRUE(SaveDatastoreSnapshot(path, old_stores).ok);

  const SnapshotSaveStage stages[] = {
      SnapshotSaveStage::kTemporaryWritten,
      SnapshotSaveStage::kTemporarySynchronized,
      SnapshotSaveStage::kSnapshotReplaced,
      SnapshotSaveStage::kDirectorySynchronized};
  for (const SnapshotSaveStage interrupted_stage : stages) {
    ASSERT_TRUE(SaveDatastoreSnapshot(path, old_stores).ok);
    const PersistenceResult result = SaveDatastoreSnapshot(
        path, new_stores, [interrupted_stage](SnapshotSaveStage stage) {
          return stage != interrupted_stage;
        });
    EXPECT_FALSE(result.ok);

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      EXPECT_EQ(entry.path().filename().string().find("state.json.tmp-"),
                std::string::npos)
          << entry.path();
    }

    DatastoreManager recovered(fixture->schema, fixture->initial);
    ASSERT_TRUE(LoadDatastoreSnapshot(path, recovered).ok);
    const std::string candidate = recovered.Read(Datastore::kCandidate).ToXml();
    if (interrupted_stage == SnapshotSaveStage::kTemporaryWritten ||
        interrupted_stage == SnapshotSaveStage::kTemporarySynchronized) {
      EXPECT_NE(candidate.find("old"), std::string::npos);
    } else {
      EXPECT_NE(candidate.find("new"), std::string::npos);
    }
  }
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

}  // namespace
}  // namespace yang::netconf
