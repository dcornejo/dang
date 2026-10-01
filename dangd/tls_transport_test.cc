// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/tls_transport.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "dangd/application.h"

namespace dangd {
namespace {

using Subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>;
using TestCertificate = std::unique_ptr<X509, decltype(&X509_free)>;

void AddCommonName(X509_NAME* subject, const unsigned char* value,
                   int length = -1) {
  ASSERT_EQ(X509_NAME_add_entry_by_NID(subject, NID_commonName, MBSTRING_UTF8,
                                       value, length, -1, 0),
            1);
}

void AddSubjectAlternativeNames(
    X509* certificate,
    const std::vector<std::pair<int, std::string>>& values) {
  GENERAL_NAMES* names = sk_GENERAL_NAME_new_null();
  ASSERT_NE(names, nullptr);
  for (const auto& [type, value] : values) {
    GENERAL_NAME* name = GENERAL_NAME_new();
    ASSERT_NE(name, nullptr);
    ASN1_IA5STRING* text = ASN1_IA5STRING_new();
    ASSERT_NE(text, nullptr);
    ASSERT_EQ(ASN1_STRING_set(text, value.data(),
                              static_cast<int>(value.size())),
              1);
    GENERAL_NAME_set0_value(name, type, text);
    ASSERT_GT(sk_GENERAL_NAME_push(names, name), 0);
  }
  ASSERT_EQ(X509_add1_ext_i2d(certificate, NID_subject_alt_name, names, 0, 0),
            1);
  GENERAL_NAMES_free(names);
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
      .host = "127.0.0.1",
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

TEST(DangdTlsTransportTest, ReusesAuthenticatedSessionAcrossCandidateRpcs) {
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
  constexpr std::array<std::string_view, 2> capabilities = {
      "urn:ietf:params:netconf:capability:candidate:1.0",
      "urn:ietf:params:netconf:capability:validate:1.1"};
  std::string error;
  auto session = TlsRpcSession::Connect(client_options, &error, capabilities);
  ASSERT_TRUE(session) << error;
  const auto rpc = [&](std::string_view body, std::string_view message_id) {
    return session->Execute(
        "<rpc xmlns='urn:ietf:params:xml:ns:netconf:base:1.0' message-id='" +
            std::string(message_id) + "'>" + std::string(body) + "</rpc>",
        &error);
  };
  EXPECT_TRUE(rpc("<lock><target><candidate/></target></lock>", "lock"))
      << error;
  EXPECT_TRUE(rpc(R"xml(
    <edit-config><target><candidate/></target><config>
      <system xmlns="urn:example:appliance">
        <hostname>session-peer</hostname>
      </system>
    </config></edit-config>)xml",
                  "edit"))
      << error;
  EXPECT_TRUE(rpc("<validate><source><candidate/></source></validate>",
                  "validate"))
      << error;
  EXPECT_TRUE(rpc("<unlock><target><candidate/></target></unlock>", "unlock"))
      << error;
  EXPECT_TRUE(rpc("<close-session/>", "close")) << error;
  session->Close();
  EXPECT_FALSE(rpc("<get/>", "after-close"));
  EXPECT_NE(error.find("session is closed"), std::string::npos) << error;
  server.join();

  EXPECT_EQ(server_result, 0) << server_diagnostics.str();
  EXPECT_NE(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kCandidate)
                .ToXml()
                .find("session-peer"),
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

TEST(DangdTlsTransportTest, SelectsExactlyOneConfiguredSanIdentity) {
  TestCertificate certificate(X509_new(), X509_free);
  ASSERT_TRUE(certificate);
  Subject subject(X509_NAME_new(), X509_NAME_free);
  ASSERT_TRUE(subject);
  AddCommonName(subject.get(),
                reinterpret_cast<const unsigned char*>("legacy-cn"));
  ASSERT_EQ(X509_set_subject_name(certificate.get(), subject.get()), 1);
  AddSubjectAlternativeNames(
      certificate.get(),
      {{GEN_DNS, "alice.example"}, {GEN_URI, "urn:example:user:alice"}});

  EXPECT_EQ(CertificateUsername(certificate.get(),
                                TlsUsernameSource::kCommonName),
            "legacy-cn");
  EXPECT_EQ(CertificateUsername(certificate.get(), TlsUsernameSource::kSanDns),
            "alice.example");
  EXPECT_EQ(CertificateUsername(certificate.get(), TlsUsernameSource::kSanUri),
            "urn:example:user:alice");

  TestCertificate ambiguous(X509_new(), X509_free);
  ASSERT_TRUE(ambiguous);
  AddSubjectAlternativeNames(
      ambiguous.get(), {{GEN_DNS, "alice"}, {GEN_DNS, "administrator"}});
  EXPECT_FALSE(CertificateUsername(ambiguous.get(),
                                   TlsUsernameSource::kSanDns));
  EXPECT_FALSE(CertificateUsername(ambiguous.get(),
                                   TlsUsernameSource::kSanUri));

  TestCertificate unsafe(X509_new(), X509_free);
  ASSERT_TRUE(unsafe);
  AddSubjectAlternativeNames(
      unsafe.get(), {{GEN_URI, std::string("urn:alice\0root", 14)}});
  EXPECT_FALSE(CertificateUsername(unsafe.get(), TlsUsernameSource::kSanUri));
  EXPECT_FALSE(CertificateUsername(nullptr, TlsUsernameSource::kSanDns));
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
