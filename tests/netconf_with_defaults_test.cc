// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/compiler.h"
#include "yang/netconf_server.h"
#include "yang/netconf_with_defaults.h"

namespace yang::netconf {
namespace {

struct DefaultsFixture {
  config::RuntimeSchema schema;
  config::ConfigDocument document;
};

std::optional<DefaultsFixture> BuildDefaultsFixture(
    VectorDiagnosticSink* diagnostics) {
  auto source = SourceFile::Create("defaults.yang", R"yang(module defaults {
    yang-version 1.1;
    namespace "urn:defaults";
    prefix d;
    container system {
      leaf hostname { type string; }
      leaf mode { type string; default "auto"; }
      leaf mtu { type uint16; default "1500"; }
      container nested { leaf enabled { type boolean; default "true"; } }
    }
  })yang", *diagnostics);
  if (!source) return std::nullopt;
  InMemoryModuleRepository repository;
  Compiler compiler(repository, *diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation) return std::nullopt;
  config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto parsed = config::ParseDatastoreXml(schema, R"xml(
    <system xmlns="urn:defaults">
      <hostname>router</hostname><mode>auto</mode>
    </system>)xml");
  if (!parsed.document) return std::nullopt;
  return DefaultsFixture{std::move(schema), std::move(*parsed.document)};
}

TEST(NetconfWithDefaultsTest, SerializesAllFourRetrievalModes) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildDefaultsFixture(&diagnostics);
  ASSERT_TRUE(fixture);

  const std::string explicit_xml = SerializeWithDefaults(
      fixture->schema, fixture->document, WithDefaultsMode::kExplicit);
  EXPECT_NE(explicit_xml.find("<mode>auto</mode>"), std::string::npos);
  EXPECT_EQ(explicit_xml.find("<mtu>"), std::string::npos);

  const std::string trimmed = SerializeWithDefaults(
      fixture->schema, fixture->document, WithDefaultsMode::kTrim);
  EXPECT_EQ(trimmed.find("<mode>"), std::string::npos);
  EXPECT_NE(trimmed.find("<hostname>router</hostname>"), std::string::npos);

  const std::string all = SerializeWithDefaults(
      fixture->schema, fixture->document, WithDefaultsMode::kReportAll);
  EXPECT_NE(all.find("<mode>auto</mode>"), std::string::npos);
  EXPECT_NE(all.find("<mtu>1500</mtu>"), std::string::npos);
  EXPECT_NE(all.find("<enabled>true</enabled>"), std::string::npos);
  EXPECT_EQ(all.find("wd:default"), std::string::npos);

  const std::string tagged = SerializeWithDefaults(
      fixture->schema, fixture->document,
      WithDefaultsMode::kReportAllTagged);
  EXPECT_NE(tagged.find("xmlns:wd=\"urn:ietf:params:xml:ns:yang:"
                        "ietf-netconf-with-defaults\""), std::string::npos);
  EXPECT_NE(tagged.find("<mtu wd:default=\"true\">1500</mtu>"),
            std::string::npos);
  EXPECT_NE(tagged.find("<enabled wd:default=\"true\">true</enabled>"),
            std::string::npos);
  EXPECT_EQ(tagged.find("<mode wd:default"), std::string::npos);
}

TEST(NetconfWithDefaultsTest, AdvertisesOnlyWhenConfiguredAndFiltersDefaults) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildDefaultsFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->document);
  NetconfServer plain(stores);
  EXPECT_EQ(plain.ServerHello(1).find("capability:with-defaults"),
            std::string::npos);

  NetconfServer server(stores, nullptr, nullptr, nullptr,
                       WithDefaultsConfig{});
  const std::string hello = server.ServerHello(2);
  EXPECT_NE(hello.find("capability:with-defaults:1.0?basic-mode=explicit"),
            std::string::npos);
  EXPECT_NE(hello.find("also-supported=report-all,report-all-tagged,trim"),
            std::string::npos);

  const RpcResponse response = server.Process("2", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="d">
      <get-config><source><running/></source>
        <filter type="subtree"><system xmlns="urn:defaults"><mtu/></system></filter>
        <with-defaults xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults">
          report-all
        </with-defaults>
      </get-config>
    </rpc>)xml");
  EXPECT_NE(response.xml.find("<mtu>1500</mtu>"), std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find("<hostname>"), std::string::npos);

  const RpcResponse xpath = server.Process("2", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="x">
      <get><filter type="xpath" xmlns:d="urn:defaults"
                   select="/d:system/d:nested/d:enabled"/>
        <with-defaults xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults">
          report-all-tagged
        </with-defaults>
      </get>
    </rpc>)xml");
  EXPECT_NE(xpath.xml.find("<d:enabled wd:default=\"true\">true</d:enabled>"),
            std::string::npos) << xpath.xml;
  EXPECT_EQ(xpath.xml.find("<d:mtu"), std::string::npos);
}

TEST(NetconfWithDefaultsTest, RejectsUnsupportedAndInvalidRequests) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildDefaultsFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->document);
  const std::string request = R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="d">
      <get><with-defaults xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults">
        report-all
      </with-defaults></get>
    </rpc>)xml";
  EXPECT_NE(NetconfServer(stores).Process("3", request).xml.find(
                "operation-not-supported"), std::string::npos);

  NetconfServer configured(stores, nullptr, nullptr, nullptr,
                           WithDefaultsConfig{});
  std::string invalid = request;
  invalid.replace(invalid.find("report-all"), 10, "unknown");
  EXPECT_NE(configured.Process("3", invalid).xml.find("invalid-value"),
            std::string::npos);
}

TEST(NetconfWithDefaultsTest, AppliesNacmAfterDefaultExpansion) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildDefaultsFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->document);
  NacmPolicy policy;
  policy.AddUserToGroup("alice", "users");
  NacmRule deny_default;
  deny_default.name = "hide-default-mtu";
  deny_default.group = "users";
  deny_default.module_name = "defaults";
  deny_default.path_prefix =
      "/{urn:defaults}system/{urn:defaults}mtu";
  deny_default.operations = AccessMask(AccessOperation::kRead);
  deny_default.action = AccessAction::kDeny;
  policy.AddRule(std::move(deny_default));
  NetconfServer server(stores, &policy, nullptr, nullptr,
                       WithDefaultsConfig{});
  const RpcResponse response = server.Process(
      {.session_id = 4, .username = "alice", .datastore_owner = "4"},
      R"xml(<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
                  message-id="nacm-default">
        <get><with-defaults
          xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults">
          report-all
        </with-defaults></get>
      </rpc>)xml");
  EXPECT_NE(response.xml.find("<hostname>router</hostname>"), std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find("<mtu>"), std::string::npos) << response.xml;
}

}  // namespace
}  // namespace yang::netconf
