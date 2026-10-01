// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction_tls.h"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
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

}  // namespace
}  // namespace dangd
