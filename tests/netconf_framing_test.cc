// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "yang/compiler.h"
#include "yang/module_resolver.h"
#include "yang/netconf_framing.h"
#include "yang/source_file.h"

namespace yang::netconf {
namespace {

TEST(NetconfFramingTest, DecodesSplitAndMultipleBase10Messages) {
  FramingDecoder decoder(BaseVersion::kBase10);
  EXPECT_TRUE(decoder.Feed("<rpc>one</rpc>]]>").messages.empty());
  DecodeResult result = decoder.Feed("]]><rpc>two</rpc>]]>]]>");
  ASSERT_EQ(result.messages.size(), 2U);
  EXPECT_EQ(result.messages[0], "<rpc>one</rpc>");
  EXPECT_EQ(result.messages[1], "<rpc>two</rpc>");
  EXPECT_FALSE(result.error);
}

TEST(NetconfFramingTest, DecodesSplitMultiChunkBase11Messages) {
  FramingDecoder decoder(BaseVersion::kBase11);
  EXPECT_TRUE(decoder.Feed("\n#4\n<rpc\n#").messages.empty());
  DecodeResult result = decoder.Feed("9\n>ok</rpc>\n##\n");
  ASSERT_EQ(result.messages.size(), 1U);
  EXPECT_EQ(result.messages[0], "<rpc>ok</rpc>");
  EXPECT_FALSE(result.error);
}

TEST(NetconfFramingTest, RejectsMalformedChunksAndLimits) {
  FramingDecoder leading_zero(BaseVersion::kBase11);
  EXPECT_TRUE(leading_zero.Feed("\n#01\nx\n##\n").error);
  EXPECT_TRUE(leading_zero.failed());
  FramingDecoder zero(BaseVersion::kBase11);
  EXPECT_TRUE(zero.Feed("\n#0\n\n##\n").error);
  FramingDecoder oversized(BaseVersion::kBase11, 3);
  EXPECT_TRUE(oversized.Feed("\n#4\ntest\n##\n").error);
  FramingDecoder excessive_feed(BaseVersion::kBase10, 4);
  EXPECT_TRUE(excessive_feed.Feed(std::string(37, 'x')).error);
}

TEST(NetconfFramingTest, EncodesBothFramingVersions) {
  EXPECT_EQ(FrameMessage(BaseVersion::kBase10, "<rpc/>"),
            "<rpc/>]]>]]>");
  EXPECT_EQ(FrameMessage(BaseVersion::kBase11, "<rpc/>"),
            "\n#6\n<rpc/>\n##\n");
}

struct SessionFixture {
  config::RuntimeSchema schema;
  config::ConfigDocument initial;
};

std::optional<SessionFixture> BuildSessionFixture(
    VectorDiagnosticSink* diagnostics) {
  auto source = SourceFile::Create("session.yang", R"yang(module session {
    yang-version 1.1; namespace "urn:session"; prefix s;
    leaf hostname { type string; mandatory true; }
  })yang", *diagnostics);
  if (!source) return std::nullopt;
  InMemoryModuleRepository repository;
  Compiler compiler(repository, *diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation) return std::nullopt;
  config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto initial = config::ParseDatastoreXml(
      schema, R"xml(<hostname xmlns="urn:session">old</hostname>)xml").document;
  if (!initial) return std::nullopt;
  return SessionFixture{std::move(schema), std::move(*initial)};
}

std::string ClientHello(bool base11) {
  std::string result =
      "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<capabilities><capability>urn:ietf:params:netconf:base:1.0"
      "</capability>";
  if (base11) {
    result += "<capability>urn:ietf:params:netconf:base:1.1</capability>";
  }
  return result + "</capabilities></hello>]]>]]>";
}

TEST(NetconfFramingTest, NegotiatesBase11AndProcessesPipelinedRpc) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildSessionFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  NetconfSession session(server, 77, "alice");
  EXPECT_NE(session.Start().find("<session-id>77</session-id>"),
            std::string::npos);
  const std::string rpc =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"1\"><get/></rpc>";
  SessionOutput output = session.Receive(ClientHello(true) +
                                         FrameMessage(BaseVersion::kBase11, rpc));
  EXPECT_TRUE(session.negotiated());
  EXPECT_EQ(session.version(), BaseVersion::kBase11);
  ASSERT_EQ(output.bytes_to_send.size(), 1U);
  EXPECT_TRUE(output.bytes_to_send[0].starts_with("\n#"));
  EXPECT_NE(output.bytes_to_send[0].find("<rpc-reply"), std::string::npos);
}

TEST(NetconfFramingTest, FallsBackToBase10AndHonorsCloseSession) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildSessionFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  NetconfSession session(server, 78, "bob");
  EXPECT_FALSE(session.Receive(ClientHello(false).substr(0, 20)).error);
  EXPECT_FALSE(session.negotiated());
  const std::string remainder = ClientHello(false).substr(20);
  SessionOutput negotiated = session.Receive(remainder);
  EXPECT_FALSE(negotiated.error);
  EXPECT_EQ(session.version(), BaseVersion::kBase10);
  const std::string close =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"2\"><close-session/></rpc>]]>]]>";
  SessionOutput output = session.Receive(close);
  EXPECT_TRUE(output.close_transport);
  ASSERT_EQ(output.bytes_to_send.size(), 1U);
  EXPECT_TRUE(output.bytes_to_send[0].ends_with("]]>]]>"));
}

TEST(NetconfFramingTest, ClosesOnMalformedHelloAndFraming) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildSessionFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  NetconfSession bad_hello(server, 79, "carol");
  SessionOutput hello = bad_hello.Receive("<rpc/>]]>]]>");
  EXPECT_TRUE(hello.error);
  EXPECT_TRUE(hello.close_transport);
  NetconfSession bad_chunk(server, 80, "dave");
  ASSERT_FALSE(bad_chunk.Receive(ClientHello(true)).error);
  SessionOutput chunk = bad_chunk.Receive("not-a-chunk");
  EXPECT_TRUE(chunk.error);
  EXPECT_TRUE(chunk.close_transport);
}

TEST(NetconfFramingTest, AcceptsPrefixedHelloAndTrimmedCapabilityText) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildSessionFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  NetconfSession session(server, 81, "eve");
  SessionOutput output = session.Receive(R"xml(
    <nc:hello xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0">
      <nc:capabilities><nc:capability>
        urn:ietf:params:netconf:base:1.1
      </nc:capability></nc:capabilities>
    </nc:hello>]]>]]>)xml");
  EXPECT_FALSE(output.error);
  EXPECT_TRUE(session.negotiated());
  EXPECT_EQ(session.version(), BaseVersion::kBase11);
}

TEST(NetconfFramingTest, KillSessionSignalsTargetAndReleasesItsLock) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildSessionFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  NetconfSession target(server, 90, "alice");
  NetconfSession administrator(server, 91, "admin");
  ASSERT_TRUE(target.valid());
  ASSERT_TRUE(administrator.valid());
  ASSERT_FALSE(target.Receive(ClientHello(false)).error);
  ASSERT_FALSE(administrator.Receive(ClientHello(false)).error);
  const std::string lock =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"lock\"><lock><target><candidate/></target></lock></rpc>"
      "]]>]]>";
  ASSERT_NE(target.Receive(lock).bytes_to_send.front().find("<ok/>"),
            std::string::npos);
  const std::string kill =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"kill\"><kill-session><session-id>90</session-id>"
      "</kill-session></rpc>]]>]]>";
  SessionOutput killed = administrator.Receive(kill);
  ASSERT_EQ(killed.bytes_to_send.size(), 1U);
  EXPECT_NE(killed.bytes_to_send.front().find("<ok/>"), std::string::npos);
  EXPECT_TRUE(target.Poll().close_transport);
  EXPECT_FALSE(server.CloseRequested(90));
  EXPECT_NE(administrator.Receive(lock).bytes_to_send.front().find("<ok/>"),
            std::string::npos);
}

TEST(NetconfFramingTest, RejectsSelfKillUnknownTargetAndDuplicateId) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildSessionFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  NetconfSession session(server, 92, "alice");
  NetconfSession duplicate(server, 92, "bob");
  EXPECT_FALSE(duplicate.valid());
  EXPECT_TRUE(duplicate.Start().empty());
  ASSERT_FALSE(session.Receive(ClientHello(false)).error);
  const auto kill = [&session](std::uint32_t target) {
    return session.Receive(
        "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
        "message-id=\"kill\"><kill-session><session-id>" +
        std::to_string(target) + "</session-id></kill-session></rpc>]]>]]>");
  };
  EXPECT_NE(kill(92).bytes_to_send.front().find("invalid-value"),
            std::string::npos);
  EXPECT_NE(kill(999).bytes_to_send.front().find("invalid-value"),
            std::string::npos);
}

TEST(NetconfFramingTest, SubscribesAndPollsFramedNotifications) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildSessionFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NotificationManager notifications;
  ASSERT_TRUE(notifications.AddStream({}));
  NetconfServer server(stores, nullptr, nullptr, &notifications);
  NetconfSession session(server, 93, "alice");
  EXPECT_NE(session.Start().find("capability:notification:1.0"),
            std::string::npos);
  ASSERT_FALSE(session.Receive(ClientHello(false)).error);
  const std::string subscribe =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"sub\"><create-subscription "
      "xmlns=\"urn:ietf:params:xml:ns:netconf:notification:1.0\"/>"
      "</rpc>]]>]]>";
  SessionOutput subscribed = session.Receive(subscribe);
  ASSERT_EQ(subscribed.bytes_to_send.size(), 1U);
  EXPECT_NE(subscribed.bytes_to_send[0].find("<ok/>"), std::string::npos);
  ASSERT_TRUE(notifications.Publish(
      "NETCONF", "events", "alarm", "<alarm xmlns=\"urn:events\"/>"));
  SessionOutput delivered = session.Poll();
  ASSERT_EQ(delivered.bytes_to_send.size(), 1U);
  EXPECT_NE(delivered.bytes_to_send[0].find("<notification"),
            std::string::npos);
  EXPECT_NE(delivered.bytes_to_send[0].find("<eventTime>"),
            std::string::npos);
  EXPECT_TRUE(delivered.bytes_to_send[0].ends_with("]]>]]>"));
}

}  // namespace
}  // namespace yang::netconf
