// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction_tls.h"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>

#include "dangd/application.h"

namespace dangd {
namespace {

std::uint16_t AvailableLoopbackPort() {
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    close(socket_fd);
    return 0;
  }
  socklen_t length = sizeof(address);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length) !=
      0) {
    close(socket_fd);
    return 0;
  }
  close(socket_fd);
  return ntohs(address.sin_port);
}

TEST(PeerTransactionTlsTest,
     ConfirmsPersistentCommitThroughProgrammaticMutualTlsClient) {
  const std::filesystem::path source = DANG_TEST_SOURCE_DIR;
  ApplicationOptions application_options{
      .model = source / "dangd/examples/appliance.yang",
      .search_paths = {source / "dangd/models"},
      .configuration = source / "dangd/examples/config.xml",
      .nacm_configuration = source / "dangd/examples/nacm.xml"};
  auto loaded = Application::Load(application_options);
  ASSERT_NE(loaded.application, nullptr)
      << testing::PrintToString(loaded.errors);

  const auto edited = loaded.application->server().Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="edit">
      <edit-config><target><candidate/></target><config>
        <system xmlns="urn:example:appliance">
          <hostname>recovered-peer</hostname>
        </system>
      </config></edit-config>
    </rpc>)xml");
  ASSERT_NE(edited.xml.find("<ok/>"), std::string::npos) << edited.xml;

  const std::filesystem::path certificates = source / "dangd/testdata/tls";
  const std::uint16_t port = AvailableLoopbackPort();
  ASSERT_NE(port, 0);
  TlsServerOptions server_options{
      .address = "127.0.0.1",
      .port = port,
      .certificate = certificates / "server-cert.pem",
      .private_key = certificates / "server-key.pem",
      .trust_anchor = certificates / "ca-cert.pem",
      .maximum_connections = 2};
  std::ostringstream server_diagnostics;
  int server_result = -1;
  std::thread server([&] {
    server_result =
        RunTlsServer(*loaded.application, server_options, server_diagnostics);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  TlsClientOptions client{.host = "localhost",
                          .port = port,
                          .certificate = certificates / "alice-cert.pem",
                          .private_key = certificates / "alice-key.pem",
                          .trust_anchor = certificates / "ca-cert.pem"};
  std::string error;
  const auto started = ExchangeTlsRpc(client, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="start">
      <commit><confirmed/><confirm-timeout>60</confirm-timeout>
        <persist>peer-token&lt;&amp;</persist></commit>
    </rpc>)xml",
                                      &error);
  EXPECT_TRUE(started) << error;
  if (started) {
    EXPECT_NE(started->reply.find("<ok/>"), std::string::npos)
        << started->reply;
  }

  PeerJournalParticipant journal_participant{
      .id = "peer-a",
      .role = PeerTransactionRole::kPrimary,
      .persistent_commit_id = "peer-token<&"};
  PeerTransactionParticipant recovery =
      MakeTlsRecoveryParticipant(journal_participant, client);
  EXPECT_FALSE(recovery.confirm());
  recovery.release();
  server.join();

  EXPECT_EQ(server_result, 0) << server_diagnostics.str();
  EXPECT_NE(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kRunning)
                .ToXml()
                .find("recovered-peer"),
            std::string::npos);
}

TEST(PeerTransactionTlsTest, RejectsUnsafeRpcBeforeConnecting) {
  TlsClientOptions unused;
  std::string error;
  EXPECT_FALSE(
      ExchangeTlsRpc(unused,
                     "<rpc xmlns='urn:ietf:params:xml:ns:netconf:base:1.0'>"
                     "</rpc>]]>]]><rpc/>",
                     &error));
  EXPECT_FALSE(error.empty());
}

TEST(PeerTransactionTlsTest, RejectsZeroSocketTimeoutBeforeConnecting) {
  const std::filesystem::path certificates =
      std::filesystem::path(DANG_TEST_SOURCE_DIR) / "dangd/testdata/tls";
  TlsClientOptions options{.host = "localhost",
                           .port = AvailableLoopbackPort(),
                           .certificate = certificates / "alice-cert.pem",
                           .private_key = certificates / "alice-key.pem",
                           .trust_anchor = certificates / "ca-cert.pem",
                           .timeout_milliseconds = 0};
  std::string error;
  EXPECT_FALSE(
      ExchangeTlsRpc(options,
                     "<rpc xmlns='urn:ietf:params:xml:ns:netconf:base:1.0' "
                     "message-id='timeout'><get/></rpc>",
                     &error));
  EXPECT_NE(error.find("timeout"), std::string::npos) << error;
}

TEST(PeerTransactionTlsTest,
     ApplicationStartupAutomaticallyRecoversEveryMappedPeer) {
  const std::filesystem::path source = DANG_TEST_SOURCE_DIR;
  const std::filesystem::path certificates = source / "dangd/testdata/tls";
  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path() /
      ("dang-peer-auto-recovery-" + std::to_string(getpid()));
  std::error_code cleanup_error;
  std::filesystem::remove_all(temporary, cleanup_error);
  ASSERT_TRUE(std::filesystem::create_directory(temporary));

  ApplicationOptions remote_options{
      .model = source / "dangd/examples/appliance.yang",
      .search_paths = {source / "dangd/models"},
      .configuration = source / "dangd/examples/config.xml",
      .nacm_configuration = source / "dangd/examples/nacm.xml"};
  auto primary = Application::Load(remote_options);
  auto standby = Application::Load(remote_options);
  ASSERT_NE(primary.application, nullptr);
  ASSERT_NE(standby.application, nullptr);

  const std::uint16_t primary_port = AvailableLoopbackPort();
  const std::uint16_t standby_port = AvailableLoopbackPort();
  ASSERT_NE(primary_port, 0);
  ASSERT_NE(standby_port, 0);
  ASSERT_NE(primary_port, standby_port);
  const auto server_options = [&](std::uint16_t port) {
    return TlsServerOptions{.address = "127.0.0.1",
                            .port = port,
                            .certificate = certificates / "server-cert.pem",
                            .private_key = certificates / "server-key.pem",
                            .trust_anchor = certificates / "ca-cert.pem",
                            .maximum_connections = 2};
  };
  std::ostringstream primary_diagnostics;
  std::ostringstream standby_diagnostics;
  int primary_result = -1;
  int standby_result = -1;
  std::thread primary_server([&] {
    primary_result =
        RunTlsServer(*primary.application, server_options(primary_port),
                     primary_diagnostics);
  });
  std::thread standby_server([&] {
    standby_result =
        RunTlsServer(*standby.application, server_options(standby_port),
                     standby_diagnostics);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto client = [&](std::uint16_t port) {
    return TlsClientOptions{.host = "localhost",
                            .port = port,
                            .certificate = certificates / "alice-cert.pem",
                            .private_key = certificates / "alice-key.pem",
                            .trust_anchor = certificates / "ca-cert.pem"};
  };
  std::string error;
  const auto arm = [&](const TlsClientOptions& target, std::string_view token) {
    return ExchangeTlsRpc(
        target,
        "<rpc xmlns='urn:ietf:params:xml:ns:netconf:base:1.0' "
        "message-id='arm'><commit><confirmed/><confirm-timeout>60</confirm-"
        "timeout><persist>" +
            std::string(token) + "</persist></commit></rpc>",
        &error);
  };
  const TlsClientOptions primary_client = client(primary_port);
  const TlsClientOptions standby_client = client(standby_port);
  EXPECT_TRUE(arm(primary_client, "primary-token")) << error;
  EXPECT_TRUE(arm(standby_client, "standby-token")) << error;

  const std::filesystem::path journal_path = temporary / "journal.json";
  PeerJournalState state{
      .transaction_id = "automatic-recovery",
      .proposal_digest = "sha256:test",
      .participants = {{.id = "primary",
                        .role = PeerTransactionRole::kPrimary,
                        .persistent_commit_id = "primary-token"},
                       {.id = "standby",
                        .role = PeerTransactionRole::kStandby,
                        .persistent_commit_id = "standby-token"}}};
  auto journal = PeerTransactionFileJournal::Create(journal_path,
                                                    std::move(state), &error);
  ASSERT_TRUE(journal) << error;
  ASSERT_EQ(journal->Callbacks()
                .record_commit_decision({"primary", "standby"})
                .status,
            PeerTransactionDecisionStatus::kCommitted);
  journal.reset();

  const std::filesystem::path recovery_path = temporary / "recovery.json";
  const auto target_json = [&](std::string id, std::uint16_t port) {
    return nlohmann::json{
        {"id", std::move(id)},
        {"host", "localhost"},
        {"port", port},
        {"certificate", (certificates / "alice-cert.pem").string()},
        {"private-key", (certificates / "alice-key.pem").string()},
        {"trust-anchor", (certificates / "ca-cert.pem").string()}};
  };
  std::ofstream recovery_output(recovery_path, std::ios::binary);
  recovery_output << nlohmann::json{
      {"version", 1},
      {"peers", nlohmann::json::array({target_json("primary", primary_port),
                                       target_json("standby", standby_port)})}};
  recovery_output.close();
  ASSERT_EQ(chmod(recovery_path.c_str(), S_IRUSR | S_IWUSR), 0);

  ApplicationOptions local_options = remote_options;
  local_options.peer_transaction_journal = journal_path;
  local_options.peer_recovery_configuration = recovery_path;
  auto recovered = Application::Load(local_options);
  EXPECT_NE(recovered.application, nullptr)
      << testing::PrintToString(recovered.errors);
  EXPECT_FALSE(std::filesystem::exists(journal_path));

  if (!recovered.application) {
    (void)ConfirmPersistentCommitOverTls(primary_client, "primary-token");
    (void)ConfirmPersistentCommitOverTls(standby_client, "standby-token");
  }
  primary_server.join();
  standby_server.join();
  EXPECT_EQ(primary_result, 0) << primary_diagnostics.str();
  EXPECT_EQ(standby_result, 0) << standby_diagnostics.str();
  std::filesystem::remove_all(temporary, cleanup_error);
}

}  // namespace
}  // namespace dangd
