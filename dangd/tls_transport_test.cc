// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/tls_transport.h"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

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

TEST(DangdTlsTransportTest, ExchangesAuthenticatedNetconfRpcOverMutualTls) {
  const std::filesystem::path source = DANG_TEST_SOURCE_DIR;
  ApplicationOptions application_options{
      .model = source / "dangd/examples/appliance.yang",
      .configuration = source / "dangd/examples/config.xml",
      .nacm_configuration = source / "dangd/examples/nacm.xml"};
  auto loaded = Application::Load(application_options);
  ASSERT_NE(loaded.application, nullptr);

  const std::filesystem::path certificates = source / "dangd/testdata/tls";
  const std::uint16_t port = AvailableLoopbackPort();
  ASSERT_NE(port, 0);
  TlsServerOptions server_options{
      .address = "127.0.0.1",
      .port = port,
      .certificate = certificates / "server-cert.pem",
      .private_key = certificates / "server-key.pem",
      .trust_anchor = certificates / "ca-cert.pem",
      .maximum_connections = 1};
  std::ostringstream server_diagnostics;
  int server_result = -1;
  std::thread server([&] {
    server_result = RunTlsServer(*loaded.application, server_options,
                                 server_diagnostics);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  TlsClientOptions client_options{
      .host = "localhost",
      .port = port,
      .certificate = certificates / "alice-cert.pem",
      .private_key = certificates / "alice-key.pem",
      .trust_anchor = certificates / "ca-cert.pem"};
  std::istringstream input(R"xml(
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
  <edit-config><target><candidate/></target><config>
    <system xmlns="urn:example:appliance"><hostname>edge-2</hostname></system>
  </config></edit-config>
</rpc>

<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="2">
  <commit/>
</rpc>

<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="3">
  <get-config><source><running/></source></get-config>
</rpc>

<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="4">
  <close-session/>
</rpc>

)xml");
  std::ostringstream output;
  std::ostringstream client_diagnostics;
  const int client_result =
      RunTlsClient(client_options, input, output, client_diagnostics);
  server.join();

  EXPECT_EQ(client_result, 0) << client_diagnostics.str();
  EXPECT_EQ(server_result, 0) << server_diagnostics.str();
  EXPECT_NE(output.str().find("<hostname>edge-2</hostname>"),
            std::string::npos);
  EXPECT_NE(output.str().find("message-id=\"4\"><ok/>"),
            std::string::npos);
}

TEST(DangdTlsTransportTest, RejectsCertificateWithoutClientAuthenticationUse) {
  const std::filesystem::path source = DANG_TEST_SOURCE_DIR;
  auto loaded = Application::Load({
      .model = source / "dangd/examples/appliance.yang",
      .configuration = source / "dangd/examples/config.xml"});
  ASSERT_NE(loaded.application, nullptr);

  const std::filesystem::path certificates = source / "dangd/testdata/tls";
  const std::uint16_t port = AvailableLoopbackPort();
  ASSERT_NE(port, 0);
  TlsServerOptions server_options{
      .address = "127.0.0.1",
      .port = port,
      .certificate = certificates / "server-cert.pem",
      .private_key = certificates / "server-key.pem",
      .trust_anchor = certificates / "ca-cert.pem",
      .maximum_connections = 1};
  std::ostringstream server_diagnostics;
  int server_result = -1;
  std::thread server([&] {
    server_result = RunTlsServer(*loaded.application, server_options,
                                 server_diagnostics);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // The server certificate chains to the CA but has only serverAuth EKU, so it
  // must not be accepted as an authenticated NETCONF client identity.
  TlsClientOptions invalid_client{
      .host = "localhost",
      .port = port,
      .certificate = certificates / "server-cert.pem",
      .private_key = certificates / "server-key.pem",
      .trust_anchor = certificates / "ca-cert.pem"};
  std::istringstream input;
  std::ostringstream output;
  std::ostringstream client_diagnostics;
  EXPECT_NE(RunTlsClient(invalid_client, input, output, client_diagnostics), 0);
  server.join();
  EXPECT_EQ(server_result, 0);
  EXPECT_NE(server_diagnostics.str().find("TLS handshake failed"),
            std::string::npos);
}

}  // namespace
}  // namespace dangd
