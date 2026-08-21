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
#include <openssl/x509.h>

#include "dangd/application.h"

namespace dangd {
namespace {

using Subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>;

void AddCommonName(X509_NAME* subject, const unsigned char* value,
                   int length = -1) {
  ASSERT_EQ(X509_NAME_add_entry_by_NID(subject, NID_commonName, MBSTRING_UTF8,
                                       value, length, -1, 0),
            1);
}

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
      .search_paths = {source / "dangd/models"},
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

TEST(DangdTlsTransportTest, MapsOnlyOneCanonicalCertificateCommonName) {
  Subject valid(X509_NAME_new(), X509_NAME_free);
  ASSERT_TRUE(valid);
  AddCommonName(valid.get(), reinterpret_cast<const unsigned char*>("alice"));
  EXPECT_EQ(CertificateSubjectUsername(valid.get()), "alice");

  Subject duplicate(X509_NAME_new(), X509_NAME_free);
  ASSERT_TRUE(duplicate);
  AddCommonName(duplicate.get(),
                reinterpret_cast<const unsigned char*>("alice"));
  AddCommonName(duplicate.get(),
                reinterpret_cast<const unsigned char*>("administrator"));
  EXPECT_FALSE(CertificateSubjectUsername(duplicate.get()));

  Subject whitespace(X509_NAME_new(), X509_NAME_free);
  ASSERT_TRUE(whitespace);
  AddCommonName(whitespace.get(),
                reinterpret_cast<const unsigned char*>(" alice"));
  EXPECT_FALSE(CertificateSubjectUsername(whitespace.get()));

  Subject embedded_nul(X509_NAME_new(), X509_NAME_free);
  ASSERT_TRUE(embedded_nul);
  const unsigned char ambiguous[] = {
      'a', 'l', 'i', 'c', 'e', 0, 'r', 'o', 'o', 't'};
  AddCommonName(embedded_nul.get(), ambiguous, sizeof(ambiguous));
  EXPECT_FALSE(CertificateSubjectUsername(embedded_nul.get()));

  Subject absent(X509_NAME_new(), X509_NAME_free);
  ASSERT_TRUE(absent);
  EXPECT_FALSE(CertificateSubjectUsername(absent.get()));
  EXPECT_FALSE(CertificateSubjectUsername(nullptr));
}

TEST(DangdTlsTransportTest, RejectsCertificateWithoutClientAuthenticationUse) {
  const std::filesystem::path source = DANG_TEST_SOURCE_DIR;
  auto loaded = Application::Load({
      .model = source / "dangd/examples/appliance.yang",
      .search_paths = {source / "dangd/models"},
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
