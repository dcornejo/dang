// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include "dangd/plugin_worker_client.h"
#include "dangd/plugin_worker_coordinator.h"
#include "dangd/plugin_worker_protocol.h"
#include "dangd/plugin_worker_runtime.h"
#include "yang/resource_limits.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>

#include <spawn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

extern char** environ;

namespace dangd {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

constexpr int kWorkerDescriptor = 198;

struct WorkerProcess {
  pid_t process = -1;
  int descriptor = -1;
  ~WorkerProcess() {
    if (descriptor >= 0) close(descriptor);
    if (process > 0) {
      kill(process, SIGKILL);
      (void)waitpid(process, nullptr, 0);
    }
  }
};

WorkerProcess SpawnWorker(const char* plugin) {
  std::array<int, 2> sockets{-1, -1};
  EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()), 0);
  posix_spawn_file_actions_t actions;
  EXPECT_EQ(posix_spawn_file_actions_init(&actions), 0);
  EXPECT_EQ(posix_spawn_file_actions_adddup2(&actions, sockets[1],
                                              kWorkerDescriptor), 0);
  EXPECT_EQ(posix_spawn_file_actions_addclose(&actions, sockets[0]), 0);
  EXPECT_NE(sockets[0], kWorkerDescriptor);
  EXPECT_NE(sockets[1], kWorkerDescriptor);
  EXPECT_EQ(posix_spawn_file_actions_addclose(&actions, sockets[1]), 0);
  std::string descriptor = std::to_string(kWorkerDescriptor);
  std::array<char*, 6> arguments{
      const_cast<char*>(DANG_TEST_PLUGIN_WORKER_PATH),
      const_cast<char*>("--socket-fd"), descriptor.data(),
      const_cast<char*>("--plugin"), const_cast<char*>(plugin), nullptr};
  pid_t process = -1;
  EXPECT_EQ(posix_spawn(&process, DANG_TEST_PLUGIN_WORKER_PATH, &actions,
                        nullptr, arguments.data(), environ), 0);
  EXPECT_EQ(posix_spawn_file_actions_destroy(&actions), 0);
  close(sockets[1]);
  return {process, sockets[0]};
}

Json ReadJson(int descriptor) {
  const WorkerReadResult response = ReadWorkerFrame(
      descriptor, std::chrono::steady_clock::now() + 5s,
      yang::DefaultResourceLimits().maximum_snapshot_bytes);
  EXPECT_EQ(response.status, WorkerIoStatus::kOk) << response.error;
  Json parsed = Json::parse(response.payload, nullptr, false);
  EXPECT_FALSE(parsed.is_discarded()) << response.payload;
  return parsed;
}

void SendJson(int descriptor, const Json& request) {
  std::string error;
  EXPECT_EQ(WriteWorkerFrame(
                descriptor, request.dump(),
                std::chrono::steady_clock::now() + 5s,
                yang::DefaultResourceLimits().maximum_snapshot_bytes, &error),
            WorkerIoStatus::kOk) << error;
}

TEST(PluginWorkerTest, OwnsPluginAndReturnsCopiedDiscoveryData) {
  WorkerProcess worker = SpawnWorker(DANG_TEST_PLUGIN_PATH);
  ASSERT_GT(worker.process, 0);
  ASSERT_GE(worker.descriptor, 0);
  const Json ready = ReadJson(worker.descriptor);
  ASSERT_TRUE(ready.value("ok", false)) << ready.dump();
  EXPECT_EQ(ready.value("stage", ""), "ready");

  std::string error;
  ASSERT_EQ(WriteWorkerFrame(
                worker.descriptor, "not-json",
                std::chrono::steady_clock::now() + 5s,
                yang::DefaultResourceLimits().maximum_snapshot_bytes, &error),
            WorkerIoStatus::kOk) << error;
  const Json rejected = ReadJson(worker.descriptor);
  EXPECT_FALSE(rejected.value("ok", true));
  EXPECT_EQ(rejected.value("stage", ""), "protocol");

  SendJson(worker.descriptor, {{"operation", "discover"}});
  const Json discovery = ReadJson(worker.descriptor);
  ASSERT_TRUE(discovery.value("ok", false)) << discovery.dump();
  ASSERT_EQ(discovery["manifests"].size(), 1U);
  EXPECT_EQ(discovery["manifests"][0]["plugin_name"],
            "dangd-example-plugin");
  EXPECT_EQ(discovery["manifests"][0]["abi_version"], DANG_PLUGIN_ABI_V1);
  ASSERT_EQ(discovery["sources"].size(), 1U);
  EXPECT_EQ(discovery["sources"][0]["module_name"], "dangd-example-plugin");
  EXPECT_NE(discovery["sources"][0]["source"].get<std::string>().find(
                "module dangd-example-plugin"),
            std::string::npos);

  SendJson(worker.descriptor, {{"operation", "shutdown"}});
  EXPECT_EQ(ReadJson(worker.descriptor).value("stage", ""), "shutdown");
  int status = 0;
  ASSERT_EQ(waitpid(worker.process, &status, 0), worker.process);
  worker.process = -1;
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(PluginWorkerTest, ReportsLoadFailureBeforeExiting) {
  WorkerProcess worker = SpawnWorker(DANG_TEST_BROKEN_PLUGIN_PATH);
  const Json failure = ReadJson(worker.descriptor);
  EXPECT_FALSE(failure.value("ok", true));
  EXPECT_EQ(failure.value("stage", ""), "load");
  ASSERT_FALSE(failure["errors"].empty());
  EXPECT_NE(failure["errors"][0].get<std::string>().find(
                "simulated discovery failure"),
            std::string::npos);
  int status = 0;
  ASSERT_EQ(waitpid(worker.process, &status, 0), worker.process);
  worker.process = -1;
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 1);
}

std::unique_ptr<PluginWorkerClient> StartClient(
    const char* plugin, std::chrono::milliseconds timeout,
    std::vector<std::string>* errors) {
  return PluginWorkerClient::Start(
      {.worker_executable = DANG_TEST_PLUGIN_WORKER_PATH,
       .plugin = plugin,
       .request_timeout = timeout},
      errors);
}

TEST(PluginWorkerClientTest, CopiesDiscoveryAndOperationalResults) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_BROKEN_OPERATIONAL_PLUGIN_PATH, 5s,
                            &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  std::string error;
  const auto discovery = client->Discover(&error);
  ASSERT_TRUE(discovery.has_value()) << error;
  EXPECT_EQ(discovery->manifest.plugin_name, "test-broken-operational");
  ASSERT_EQ(discovery->sources.size(), 1U);
  EXPECT_EQ(discovery->sources[0].module_name,
            "dangd-test-broken-operational");
  const PluginWorkerOperationalResult operational = client->OperationalData();
  ASSERT_FALSE(operational.worker_error.has_value())
      << *operational.worker_error;
  ASSERT_EQ(operational.fragments.size(), 1U);
  EXPECT_EQ(operational.fragments[0].provider,
            "test-broken-operational");
  EXPECT_NE(operational.fragments[0].data_xml.find("invalid"),
            std::string::npos);
  EXPECT_TRUE(client->healthy());
}

TEST(PluginWorkerClientTest, CopiesAndVerifiesPeerTransactionContract) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PROVIDER_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  std::string discovery_error;
  const auto discovery = client->Discover(&discovery_error);
  ASSERT_TRUE(discovery.has_value()) << discovery_error;
  EXPECT_EQ(discovery->manifest.abi_version, DANG_PLUGIN_ABI_V9);
  EXPECT_TRUE(discovery->manifest.supports_peer_transactions);
  ASSERT_TRUE(client
                  ->Prepare("<config/>",
                            "<config><provider-settings>"
                            "<mode>peer-plan-valid</mode>"
                            "</provider-settings></config>",
                            "[]")
                  .ok());
  ASSERT_TRUE(client->Validate().ok());
  const auto planned = client->PeerCandidates();
  ASSERT_TRUE(planned.ok()) << planned.worker_error.value_or("");
  ASSERT_EQ(planned.candidates.size(), 2u);
  EXPECT_EQ(planned.candidates.front().group_id, "test-group");
  EXPECT_EQ(planned.candidates.front().module_name, "dangd-test-provider");
  EXPECT_EQ(planned.candidates.front().role, DANG_PEER_PRIMARY_V1);
  PluginPeerVerification verification{
      .provider = "test-provider",
      .group_id = "test-group",
      .participant_id = "primary",
      .verification_context_json = "{\"expected_status\":\"ready\"}",
      .running_reply_xml = "<rpc-reply><data/></rpc-reply>",
      .operational_reply_xml = "<rpc-reply><data>ready</data></rpc-reply>"};
  EXPECT_TRUE(client->VerifyPeer(verification).ok());
  verification.operational_reply_xml =
      "<rpc-reply><data>waiting</data></rpc-reply>";
  const auto pending = client->VerifyPeer(verification);
  ASSERT_FALSE(pending.ok());
  ASSERT_TRUE(pending.finding.has_value());
  EXPECT_EQ(pending.finding->netconf_error_app_tag,
            "peer-verification-pending");
  verification.operational_reply_xml = "<rpc-reply><data/></rpc-reply>";
  const auto rejected = client->VerifyPeer(verification);
  ASSERT_FALSE(rejected.ok());
  ASSERT_TRUE(rejected.finding.has_value());
  EXPECT_NE(rejected.finding->message.find("did not report ready"),
            std::string::npos);
  EXPECT_FALSE(client->Abort().has_value());
}

TEST(PluginWorkerClientTest, TerminatesAndReapsTimedOutWorker) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_HANGING_OPERATIONAL_PLUGIN_PATH, 50ms,
                            &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  const PluginWorkerOperationalResult result = client->OperationalData();
  ASSERT_TRUE(result.worker_error.has_value());
  EXPECT_NE(result.worker_error->find("timed out"), std::string::npos);
  EXPECT_TRUE(result.fragments.empty());
  EXPECT_FALSE(client->healthy());
}

TEST(PluginWorkerClientTest, ContainsAndReapsCrashedWorker) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_CRASHING_OPERATIONAL_PLUGIN_PATH, 5s,
                            &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  const PluginWorkerOperationalResult result = client->OperationalData();
  ASSERT_TRUE(result.worker_error.has_value());
  EXPECT_NE(result.worker_error->find("worker exited"), std::string::npos)
      << *result.worker_error;
  EXPECT_TRUE(result.fragments.empty());
  EXPECT_FALSE(client->healthy());
}

TEST(PluginWorkerClientTest, RetainsPreparationAcrossValidateAndAbort) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  EXPECT_TRUE(client->Prepare("<config/>", "<config><mode>ok</mode></config>",
                              "[]").ok());
  EXPECT_TRUE(client->Validate().ok());
  EXPECT_FALSE(client->Abort().has_value());

  const PluginWorkerTransactionResult missing = client->Validate();
  ASSERT_TRUE(missing.finding.has_value());
  EXPECT_NE(missing.finding->message.find("no prepared transaction"),
            std::string::npos);
  EXPECT_TRUE(client->healthy());
}

TEST(PluginWorkerClientTest, ReturnsAttributedValidationRejection) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  ASSERT_TRUE(client->Prepare(
      "<config/>", "<config><mode>reject</mode></config>", "[]").ok());
  const PluginWorkerTransactionResult rejected = client->Validate();
  ASSERT_TRUE(rejected.finding.has_value());
  EXPECT_NE(rejected.finding->message.find("not supported"), std::string::npos);
  EXPECT_EQ(rejected.finding->instance_path,
            "/dep:plugin-settings/dep:mode");
  EXPECT_EQ(rejected.finding->module_name, "dangd-example-plugin");
  EXPECT_TRUE(client->healthy());
}

TEST(PluginWorkerClientTest, ExecutesSyntheticLegacyHardwareAction) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  ASSERT_TRUE(client->Prepare("<config><mode>before</mode></config>",
                              "<config><mode>after</mode></config>", "[]").ok());
  ASSERT_TRUE(client->Validate().ok());
  const PluginWorkerHardwareActionsResult planned = client->HardwareActions();
  ASSERT_TRUE(planned.ok());
  ASSERT_EQ(planned.actions.size(), 1u);
  EXPECT_EQ(planned.actions.front().action_id, "transaction");
  EXPECT_EQ(planned.actions.front().action_class, DANG_HARDWARE_NORMAL_V1);
  EXPECT_TRUE(client->ApplyAction("transaction").ok());
  EXPECT_TRUE(client->RollbackAction("transaction").ok());
  EXPECT_FALSE(client->Abort().has_value());
}

TEST(PluginWorkerClientTest, RejectsUnknownLegacyHardwareAction) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  ASSERT_TRUE(client->Prepare("<config/>", "<config/>", "[]").ok());
  const PluginWorkerTransactionResult rejected = client->ApplyAction("other");
  ASSERT_TRUE(rejected.finding.has_value());
  EXPECT_NE(rejected.finding->message.find("apply failed"), std::string::npos);
  EXPECT_EQ(rejected.finding->module_name, "dangd-example-plugin");
  EXPECT_TRUE(client->healthy());
}

TEST(PluginWorkerClientTest, CopiesAndExecutesAbiV4HardwareAction) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PROVIDER_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  ASSERT_TRUE(client->Prepare("<config/>", "<config/>", "[]").ok());
  ASSERT_TRUE(client->Validate().ok());
  const PluginWorkerHardwareActionsResult planned = client->HardwareActions();
  ASSERT_TRUE(planned.ok());
  ASSERT_EQ(planned.actions.size(), 1u);
  EXPECT_EQ(planned.actions.front().action_id, "transaction");
  EXPECT_EQ(planned.actions.front().instance_path, "");
  EXPECT_EQ(planned.actions.front().action_class, DANG_HARDWARE_NORMAL_V1);
  EXPECT_TRUE(planned.actions.front().dependencies.empty());
  EXPECT_TRUE(client->ApplyAction("transaction").ok());
  EXPECT_TRUE(client->RollbackAction("transaction").ok());
  EXPECT_FALSE(client->Abort().has_value());
}

TEST(PluginWorkerCoordinatorTest, PlansAndAppliesOneWorker) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  std::string discovery_error;
  auto discovery = client->Discover(&discovery_error);
  ASSERT_TRUE(discovery.has_value()) << discovery_error;
  ASSERT_TRUE(client->Prepare("<config/>", "<config/>", "[]").ok());
  ASSERT_TRUE(client->Validate().ok());
  PluginWorkerCoordinator coordinator;
  const HardwareTransactionResult planned =
      coordinator.Plan({{discovery->manifest, client.get()}});
  ASSERT_TRUE(planned.ok) << planned.message;
  EXPECT_EQ(planned.execution_order,
            std::vector<std::string>{"dangd-example-plugin:transaction"});
  const HardwareTransactionResult applied = coordinator.Apply();
  EXPECT_TRUE(applied.ok) << applied.message;
  const yang::config::RuntimeSchema empty_schema;
  const yang::config::ConfigDocument empty_configuration;
  const PluginApplyResult reconciled =
      coordinator.Reconcile(empty_schema, empty_configuration);
  EXPECT_FALSE(reconciled.error.has_value());
  EXPECT_TRUE(reconciled.applied.has_value());
  EXPECT_TRUE(reconciled.outcomes.empty());
  EXPECT_EQ(reconciled.applied_xml,
            "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" />\n");
}

TEST(PluginWorkerCoordinatorTest, AddsCrossModuleDependencyEdges) {
  std::vector<std::string> errors;
  auto provider = StartClient(DANG_TEST_PROVIDER_PLUGIN_PATH, 5s, &errors);
  auto consumer = StartClient(DANG_TEST_CONSUMER_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(provider, nullptr) << testing::PrintToString(errors);
  ASSERT_NE(consumer, nullptr) << testing::PrintToString(errors);
  std::string discovery_error;
  auto provider_discovery = provider->Discover(&discovery_error);
  ASSERT_TRUE(provider_discovery.has_value()) << discovery_error;
  auto consumer_discovery = consumer->Discover(&discovery_error);
  ASSERT_TRUE(consumer_discovery.has_value()) << discovery_error;
  for (PluginWorkerClient* client : {provider.get(), consumer.get()}) {
    ASSERT_TRUE(client->Prepare("<config/>", "<config/>", "[]").ok());
    ASSERT_TRUE(client->Validate().ok());
  }
  PluginWorkerCoordinator coordinator;
  const HardwareTransactionResult planned = coordinator.Plan(
      {{consumer_discovery->manifest, consumer.get()},
       {provider_discovery->manifest, provider.get()}});
  ASSERT_TRUE(planned.ok) << planned.message;
  EXPECT_EQ(planned.execution_order,
            (std::vector<std::string>{"test-provider:transaction",
                                      "test-consumer:transaction"}));
  const HardwareTransactionResult applied = coordinator.Apply();
  EXPECT_FALSE(applied.ok);
  EXPECT_NE(applied.message.find("provider was not applied first"),
            std::string::npos);
  EXPECT_TRUE(applied.rollback_failures.empty());
  EXPECT_FALSE(provider->Abort().has_value());
  EXPECT_FALSE(consumer->Abort().has_value());
}

TEST(PluginWorkerRuntimeTest, LoadsDiscoveryAndRoutesOperations) {
  std::vector<std::string> errors;
  auto runtime = PluginWorkerRuntime::Load(
      DANG_TEST_PLUGIN_WORKER_PATH,
      {DANG_TEST_PROVIDER_PLUGIN_PATH, DANG_TEST_CONSUMER_PLUGIN_PATH},
      &errors);
  ASSERT_NE(runtime, nullptr) << testing::PrintToString(errors);
  EXPECT_EQ(runtime->manifests().size(), 2u);
  EXPECT_EQ(runtime->yang_sources().size(), 2u);
  EXPECT_EQ(runtime->manifests().front().resource_domains,
            std::vector<std::string>{"routing"});
  const auto events = runtime->Notifications();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events.front().provider, "test-provider");
  EXPECT_EQ(events.front().module_name, "dangd-test-provider");
  EXPECT_EQ(events.front().notification_name, "provider-event");
  EXPECT_NE(events.front().content_xml.find("<status>ready</status>"),
            std::string::npos);
  EXPECT_TRUE(runtime->Notifications().empty());
  const auto fragments = runtime->OperationalData();
  ASSERT_EQ(fragments.size(), 1u);
  EXPECT_TRUE(std::ranges::none_of(fragments, [](const auto& fragment) {
    return fragment.error.has_value();
  }));
  yang::config::RuntimeSchemaNode operation;
  operation.module_name = "dangd-test-provider";
  operation.name.local_name = "provider-status";
  const auto invoked = runtime->InvokeRpc({}, operation, "<provider-status/>");
  ASSERT_TRUE(invoked.result.ok);
  EXPECT_NE(invoked.output_xml.find(">ready</status>"), std::string::npos);
}

TEST(PluginWorkerRuntimeTest, RejectsDuplicateResourceOwnership) {
  std::vector<std::string> errors;
  auto runtime = PluginWorkerRuntime::Load(
      DANG_TEST_PLUGIN_WORKER_PATH,
      {DANG_TEST_PROVIDER_PLUGIN_PATH,
       DANG_TEST_RESOURCE_CONFLICT_PLUGIN_PATH},
      &errors);
  EXPECT_EQ(runtime, nullptr);
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.back().find("resource domain routing is owned by plugins"),
            std::string::npos);
}

TEST(PluginWorkerCoordinatorTest, DefensivelyRejectsDuplicateResourceOwners) {
  std::vector<std::string> errors;
  auto provider = StartClient(DANG_TEST_PROVIDER_PLUGIN_PATH, 5s, &errors);
  auto conflict =
      StartClient(DANG_TEST_RESOURCE_CONFLICT_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(provider, nullptr) << testing::PrintToString(errors);
  ASSERT_NE(conflict, nullptr) << testing::PrintToString(errors);
  std::string discovery_error;
  auto provider_discovery = provider->Discover(&discovery_error);
  auto conflict_discovery = conflict->Discover(&discovery_error);
  ASSERT_TRUE(provider_discovery.has_value()) << discovery_error;
  ASSERT_TRUE(conflict_discovery.has_value()) << discovery_error;
  PluginWorkerCoordinator coordinator;
  const HardwareTransactionResult planned = coordinator.Plan(
      {{provider_discovery->manifest, provider.get()},
       {conflict_discovery->manifest, conflict.get()}});
  EXPECT_FALSE(planned.ok);
  EXPECT_NE(planned.message.find(
                "resource domain routing has more than one plugin worker owner"),
            std::string::npos);
}

TEST(PluginWorkerRuntimeTest, RejectsMissingRuntimeDependency) {
  std::vector<std::string> errors;
  auto runtime = PluginWorkerRuntime::Load(
      DANG_TEST_PLUGIN_WORKER_PATH, {DANG_TEST_CONSUMER_PLUGIN_PATH}, &errors);
  EXPECT_EQ(runtime, nullptr);
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.back().find("requires missing implementation module"),
            std::string::npos);
}

TEST(PluginWorkerRuntimeTest, RestartsCrashedWorkerForNextIndependentRequest) {
  std::vector<std::string> errors;
  auto runtime = PluginWorkerRuntime::Load(
      DANG_TEST_PLUGIN_WORKER_PATH,
      {DANG_TEST_CRASHING_OPERATIONAL_PLUGIN_PATH}, &errors);
  ASSERT_NE(runtime, nullptr) << testing::PrintToString(errors);

  // The first publication crashes its worker. The request is reported as a
  // failure and is deliberately not replayed because callbacks may have
  // external side effects.
  const auto first = runtime->OperationalData();
  ASSERT_EQ(first.size(), 1u);
  ASSERT_TRUE(first.front().error.has_value());
  EXPECT_NE(first.front().error->find("worker exited"), std::string::npos);

  // A later independent publication gets a newly spawned, rediscovered worker.
  // This deliberately crashing fixture therefore exits again; receiving the
  // crash error rather than "not available" proves that restart occurred.
  const auto second = runtime->OperationalData();
  ASSERT_EQ(second.size(), 1u);
  ASSERT_TRUE(second.front().error.has_value());
  EXPECT_NE(second.front().error->find("worker exited"), std::string::npos);
  EXPECT_EQ(second.front().error->find("not available"), std::string::npos);
}

TEST(PluginWorkerRuntimeTest, RestartsTimedOutWorkerForNextIndependentRequest) {
  std::vector<std::string> errors;
  auto runtime = PluginWorkerRuntime::Load(
      DANG_TEST_PLUGIN_WORKER_PATH,
      {DANG_TEST_HANGING_OPERATIONAL_PLUGIN_PATH}, &errors, 5s, 50ms);
  ASSERT_NE(runtime, nullptr) << testing::PrintToString(errors);

  const auto first = runtime->OperationalData();
  ASSERT_EQ(first.size(), 1u);
  ASSERT_TRUE(first.front().error.has_value());
  EXPECT_NE(first.front().error->find("timed out"), std::string::npos);

  const auto second = runtime->OperationalData();
  ASSERT_EQ(second.size(), 1u);
  ASSERT_TRUE(second.front().error.has_value());
  EXPECT_NE(second.front().error->find("timed out"), std::string::npos);
  EXPECT_EQ(second.front().error->find("not available"), std::string::npos);
}

TEST(PluginWorkerClientTest, CopiesAbiV6AppliedStateReport) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PROVIDER_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  const std::string requested =
      "<config><provider-settings xmlns=\"urn:dangd:test:provider\">"
      "<mode>backend-transform</mode></provider-settings></config>";
  ASSERT_TRUE(client->Prepare("<config/>", requested, "[]").ok());
  ASSERT_TRUE(client->Validate().ok());
  const PluginWorkerReconcileResult reconciled = client->Reconcile(requested);
  ASSERT_TRUE(reconciled.ok());
  EXPECT_NE(reconciled.report->applied_xml.find("device-normalized"),
            std::string::npos);
  ASSERT_EQ(reconciled.report->outcomes.size(), 3u);
  EXPECT_EQ(reconciled.report->outcomes.front().provider, "test-provider");
  EXPECT_EQ(reconciled.report->outcomes.front().disposition,
            DANG_CONFIGURATION_TRANSFORMED_V1);
  EXPECT_FALSE(client->Abort().has_value());
}

TEST(PluginWorkerClientTest, LegacyReconcileReturnsUnchangedSnapshot) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  ASSERT_TRUE(client->Prepare("<config/>", "<config/>", "[]").ok());
  const PluginWorkerReconcileResult reconciled =
      client->Reconcile("<config><unchanged/></config>");
  ASSERT_TRUE(reconciled.ok());
  EXPECT_EQ(reconciled.report->applied_xml,
            "<config><unchanged/></config>");
  EXPECT_TRUE(reconciled.report->outcomes.empty());
  EXPECT_FALSE(client->Abort().has_value());
}

TEST(PluginWorkerClientTest, InvokesOperationAndCopiesOutput) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PROVIDER_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  const PluginWorkerOperationResult invoked = client->Invoke(
      "dangd-test-provider", "provider-status", "", "<provider-status/>");
  ASSERT_TRUE(invoked.ok()) << invoked.worker_error.value_or("");
  EXPECT_NE(invoked.operation.output_xml.find(">ready</status>"),
            std::string::npos);
}

TEST(PluginWorkerClientTest, ReturnsAttributedUnsupportedOperation) {
  std::vector<std::string> errors;
  auto client = StartClient(DANG_TEST_PLUGIN_PATH, 5s, &errors);
  ASSERT_NE(client, nullptr) << testing::PrintToString(errors);
  const PluginWorkerOperationResult invoked = client->Invoke(
      "dangd-example-plugin", "missing", "", "<missing/>");
  ASSERT_FALSE(invoked.ok());
  ASSERT_EQ(invoked.operation.result.errors.size(), 1u);
  EXPECT_EQ(invoked.operation.result.errors.front().module_name,
            "dangd-example-plugin");
  EXPECT_EQ(invoked.operation.result.errors.front().netconf_error_tag,
            "operation-not-supported");
  EXPECT_TRUE(client->healthy());
}

}  // namespace
}  // namespace dangd
