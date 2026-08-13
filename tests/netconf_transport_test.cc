// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "yang/compiler.h"
#include "yang/module_resolver.h"
#include "yang/netconf_transport.h"
#include "yang/source_file.h"

namespace yang::netconf {
namespace {

class MemoryStream final : public SecureByteStream {
 public:
  WriteStatus Write(std::string_view bytes) override {
    if (status != WriteStatus::kAccepted) return status;
    writes.emplace_back(bytes);
    return WriteStatus::kAccepted;
  }
  void Close() override { closed = true; }
  WriteStatus status = WriteStatus::kAccepted;
  std::vector<std::string> writes;
  bool closed = false;
};

struct Fixture {
  config::RuntimeSchema schema;
  config::ConfigDocument initial;
};

std::optional<Fixture> BuildFixture(VectorDiagnosticSink* diagnostics) {
  auto source = SourceFile::Create("transport.yang", R"yang(module transport {
    yang-version 1.1; namespace "urn:transport"; prefix t;
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
      schema, "<value xmlns=\"urn:transport\">old</value>").document;
  if (!initial) return std::nullopt;
  return Fixture{std::move(schema), std::move(*initial)};
}

TransportIdentity SshIdentity(std::string username = "alice") {
  return {SecureTransport::kSsh, std::move(username), {"operators"},
          "netconf", true};
}

TEST(NetconfTransportTest, AcceptsAuthenticatedSshAndTlsSessions) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  MemoryStream ssh_stream;
  NetconfTransportAdapter ssh(server, ssh_stream, 201, SshIdentity());
  ASSERT_TRUE(ssh.valid());
  ASSERT_EQ(ssh_stream.writes.size(), 1U);
  EXPECT_NE(ssh_stream.writes[0].find("<hello"), std::string::npos);

  MemoryStream tls_stream;
  TransportIdentity tls{SecureTransport::kTls, "certificate-user", {}, "", true};
  NetconfTransportAdapter tls_adapter(server, tls_stream, 202, std::move(tls));
  EXPECT_TRUE(tls_adapter.valid());
  EXPECT_FALSE(tls_stream.closed);
}

TEST(NetconfTransportTest, RejectsUnauthenticatedInvalidSubsystemAndUsername) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  MemoryStream unauthenticated_stream;
  TransportIdentity unauthenticated = SshIdentity();
  unauthenticated.peer_authenticated = false;
  NetconfTransportAdapter unauthenticated_adapter(
      server, unauthenticated_stream, 203, std::move(unauthenticated));
  EXPECT_FALSE(unauthenticated_adapter.valid());
  EXPECT_TRUE(unauthenticated_stream.closed);

  MemoryStream subsystem_stream;
  TransportIdentity subsystem = SshIdentity();
  subsystem.ssh_subsystem = "shell";
  NetconfTransportAdapter wrong_subsystem(
      server, subsystem_stream, 204, std::move(subsystem));
  EXPECT_FALSE(wrong_subsystem.valid());

  MemoryStream username_stream;
  NetconfTransportAdapter invalid_username(
      server, username_stream, 205, SshIdentity(std::string("a\x01b", 3)));
  EXPECT_FALSE(invalid_username.valid());
}

TEST(NetconfTransportTest, RetriesBackpressureAndEnforcesQueueLimit) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  MemoryStream stream;
  stream.status = WriteStatus::kWouldBlock;
  TransportLimits limits;
  limits.maximum_queued_messages = 2;
  NetconfTransportAdapter adapter(server, stream, 206, SshIdentity(), limits);
  EXPECT_TRUE(adapter.valid());
  EXPECT_GT(adapter.queued_bytes(), 0U);
  stream.status = WriteStatus::kAccepted;
  adapter.Poll();
  EXPECT_EQ(adapter.queued_bytes(), 0U);
  EXPECT_EQ(stream.writes.size(), 1U);

  MemoryStream blocked;
  blocked.status = WriteStatus::kWouldBlock;
  limits.maximum_queued_messages = 1;
  NetconfTransportAdapter overflowing(
      server, blocked, 207, SshIdentity(), limits);
  ASSERT_TRUE(overflowing.valid());
  overflowing.Receive(
      "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<capabilities><capability>urn:ietf:params:netconf:base:1.0"
      "</capability></capabilities></hello>]]>]]>"
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"1\"><get/></rpc>]]>]]>");
  EXPECT_FALSE(overflowing.valid());
  EXPECT_TRUE(blocked.closed);
  ASSERT_TRUE(overflowing.error());
  EXPECT_NE(overflowing.error()->find("queue limit"), std::string_view::npos);
}

TEST(NetconfTransportTest, EnforcesTimeoutCancellationAndEofCleanup) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  MemoryStream timed_stream;
  TransportLimits limits;
  limits.inactivity_timeout = std::chrono::seconds(5);
  const auto start = NetconfTransportAdapter::Clock::now();
  NetconfTransportAdapter timed(
      server, timed_stream, 208, SshIdentity(), limits, start);
  timed.Poll(start + std::chrono::seconds(5));
  EXPECT_FALSE(timed.valid());
  EXPECT_TRUE(timed_stream.closed);

  MemoryStream cancelled_stream;
  NetconfTransportAdapter cancelled(
      server, cancelled_stream, 209, SshIdentity());
  cancelled.Cancel();
  cancelled.Cancel();
  EXPECT_TRUE(cancelled_stream.closed);
  EXPECT_FALSE(cancelled.valid());

  MemoryStream eof_stream;
  NetconfTransportAdapter eof(server, eof_stream, 210, SshIdentity());
  ASSERT_EQ(server.Sessions().size(), 1U);
  eof.TransportClosed();
  EXPECT_TRUE(server.Sessions().empty());
}

TEST(NetconfTransportTest, FlushesCloseSessionReplyBeforeClosingTransport) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  MemoryStream stream;
  NetconfTransportAdapter adapter(server, stream, 211, SshIdentity());
  adapter.Receive(
      "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<capabilities><capability>urn:ietf:params:netconf:base:1.0"
      "</capability></capabilities></hello>]]>]]>"
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"9\"><close-session/></rpc>]]>]]>");
  ASSERT_GE(stream.writes.size(), 2U);
  EXPECT_NE(stream.writes.back().find("message-id=\"9\"><ok/>"),
            std::string::npos);
  EXPECT_TRUE(stream.closed);
}

TEST(NetconfTransportTest, SelectsCallHomeDefaultPortsAndValidatesTarget) {
  CallHomeTarget observed;
  const CallHomeConnector connector = [&](const CallHomeTarget& target,
                                           std::string*) {
    observed = target;
    return std::make_unique<MemoryStream>();
  };
  std::string error;
  EXPECT_TRUE(ConnectCallHome({"manager.example", 0, SecureTransport::kSsh},
                              connector, &error));
  EXPECT_EQ(observed.port, 4334);
  EXPECT_TRUE(ConnectCallHome({"manager.example", 0, SecureTransport::kTls},
                              connector, &error));
  EXPECT_EQ(observed.port, 4335);
  EXPECT_FALSE(ConnectCallHome({"", 0, SecureTransport::kTls}, connector,
                               &error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace yang::netconf
