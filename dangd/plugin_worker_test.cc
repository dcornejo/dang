// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include "dangd/plugin_worker_client.h"
#include "dangd/plugin_worker_protocol.h"
#include "yang/resource_limits.h"

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

}  // namespace
}  // namespace dangd
