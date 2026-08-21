// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "yang/compiler.h"
#include "yang/module_resolver.h"
#include "yang/netconf_server.h"
#include "yang/source_file.h"

namespace yang::netconf {
namespace {

class MemoryUrlProvider final : public UrlDatastoreProvider {
 public:
  std::vector<std::string> Schemes() const override { return {"memory"}; }
  UrlResult Read(std::string_view url) override {
    const auto found = values.find(std::string(url));
    return found == values.end() ? UrlResult{{}, "not found"}
                                 : UrlResult{found->second, {}};
  }
  UrlResult Write(std::string_view url, std::string_view xml) override {
    values[std::string(url)] = std::string(xml);
    return {};
  }
  UrlResult Delete(std::string_view url) override {
    values.erase(std::string(url));
    return {};
  }
  std::map<std::string, std::string> values;
};

class RecordingOperationProvider final : public OperationProvider {
 public:
  OperationResult InvokeRpc(const RpcSessionContext&,
      const config::RuntimeSchemaNode& operation,
      std::string_view operation_xml) override {
    called = operation.module_name + ":" + operation.name.local_name;
    input = operation_xml;
    return {{true, {}, {}},
            R"xml(<result xmlns="urn:rpc-test">pong</result>)xml"};
  }
  OperationResult InvokeAction(const RpcSessionContext&,
      const config::RuntimeSchemaNode& action, std::string_view instance_path,
      std::string_view action_xml) override {
    called = action.module_name + ":" + action.name.local_name;
    path = instance_path;
    input = action_xml;
    return {{true, {}, {}},
            R"xml(<status xmlns="urn:rpc-test">reset</status>)xml"};
  }
  std::string called;
  std::string path;
  std::string input;
};

class BlockingOperationProvider final : public OperationProvider {
 public:
  OperationResult InvokeRpc(const RpcSessionContext&,
      const config::RuntimeSchemaNode&, std::string_view) override {
    std::unique_lock lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
    return {{true, {}, {}},
            R"xml(<result xmlns="urn:rpc-test">snapshot</result>)xml"};
  }

  void WaitUntilEntered() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return entered_; });
  }

  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false;
  bool released_ = false;
};

struct ServerFixture {
  config::RuntimeSchema schema;
  config::ConfigDocument initial;
};

std::optional<ServerFixture> BuildServerFixture(
    VectorDiagnosticSink* diagnostics) {
  auto source = SourceFile::Create("rpc.yang", R"yang(module rpc {
    yang-version 1.1; namespace "urn:rpc-test"; prefix r;
    container system {
      leaf hostname { type string; mandatory true; }
      action reset { output { leaf status { type string; } } }
    }
    list device {
      key name;
      leaf name { type string; }
      action bounce { output { leaf status { type string; } } }
    }
    rpc ping {
      input { leaf count { type uint16; mandatory true; } }
      output { leaf result { type string; } }
    }
  })yang", *diagnostics);
  if (!source) return std::nullopt;
  InMemoryModuleRepository repository;
  Compiler compiler(repository, *diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation) return std::nullopt;
  config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto initial = config::ParseDatastoreXml(
      schema, R"xml(<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
        <system xmlns="urn:rpc-test"><hostname>old</hostname></system>
        <device xmlns="urn:rpc-test"><name>edge-1</name></device>
      </config>)xml")
                     .document;
  if (!initial) return std::nullopt;
  return ServerFixture{std::move(schema), std::move(*initial)};
}

TEST(NetconfServerTest, AdvertisesImplementedCapabilities) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  const std::string hello = server.ServerHello(42);
  EXPECT_NE(hello.find("<session-id>42</session-id>"), std::string::npos);
  EXPECT_NE(hello.find("capability:candidate:1.0"), std::string::npos);
  EXPECT_NE(hello.find("capability:confirmed-commit:1.1"), std::string::npos);
  EXPECT_NE(hello.find("capability:xpath:1.0"), std::string::npos);
}

TEST(NetconfServerTest, AuthorizesAndDispatchesSchemaRpc) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy nacm;
  nacm.set_exec_default(AccessAction::kDeny);
  nacm.set_read_default(AccessAction::kDeny);
  RecordingOperationProvider operations;
  NetconfServer denied(stores, &nacm, nullptr, nullptr, std::nullopt, nullptr,
                       &operations);
  const std::string request = R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="rpc">
      <ping xmlns="urn:rpc-test"><count>7</count></ping>
    </rpc>)xml";
  const RpcResponse denied_rpc = denied.Process("alice", request);
  EXPECT_NE(denied_rpc.xml.find("access-denied"), std::string::npos);
  EXPECT_NE(denied_rpc.xml.find(
                "xmlns:op=\"urn:rpc-test\">/nc:rpc/op:ping</error-path>"),
            std::string::npos) << denied_rpc.xml;
  EXPECT_TRUE(operations.called.empty());

  nacm.AddUserToGroup("alice", "operators");
  nacm.AddRule({"ping", "operators", "ping", "",
                AccessMask(AccessOperation::kExecute),
                AccessAction::kPermit, "rpc"});
  nacm.AddRule({"read-rpc-output", "operators", "", "",
                AccessMask(AccessOperation::kRead),
                AccessAction::kPermit, "rpc"});
  const RpcResponse allowed = denied.Process("alice", request);
  EXPECT_EQ(operations.called, "rpc:ping");
  EXPECT_NE(allowed.xml.find("pong"), std::string::npos) << allowed.xml;

  operations.called.clear();
  const RpcResponse invalid = denied.Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="bad">
      <ping xmlns="urn:rpc-test"><count>not-a-number</count></ping>
    </rpc>)xml");
  EXPECT_NE(invalid.xml.find("invalid-value"), std::string::npos) << invalid.xml;
  EXPECT_TRUE(operations.called.empty());
}

TEST(NetconfServerTest, UsesOneNacmSnapshotForEntireRpcDuringPolicyReplacement) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy policy;
  policy.set_exec_default(AccessAction::kPermit);
  policy.set_read_default(AccessAction::kDeny);
  BlockingOperationProvider operations;
  NetconfServer server(stores, &policy, nullptr, nullptr, std::nullopt, nullptr,
                       &operations);
  const std::string request = R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="snapshot">
      <ping xmlns="urn:rpc-test"><count>7</count></ping>
    </rpc>)xml";

  RpcResponse response;
  std::thread request_thread(
      [&] { response = server.Process("alice", request); });
  operations.WaitUntilEntered();
  NacmPolicy replacement;
  replacement.set_exec_default(AccessAction::kDeny);
  replacement.set_read_default(AccessAction::kPermit);
  replacement.PreserveRuntimeStateFrom(policy);
  policy = std::move(replacement);
  operations.Release();
  request_thread.join();

  EXPECT_EQ(response.xml.find("<result"), std::string::npos) << response.xml;
  const RpcResponse subsequent = server.Process("alice", request);
  EXPECT_NE(subsequent.xml.find("access-denied"), std::string::npos)
      << subsequent.xml;
}

TEST(NetconfServerTest, RequiresReadableAncestorsBeforeDispatchingAction) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy nacm;
  nacm.AddUserToGroup("alice", "operators");
  nacm.set_read_default(AccessAction::kDeny);
  nacm.AddRule({"reset", "operators", "", "/{urn:rpc-test}system/"
                "{urn:rpc-test}reset", AccessMask(AccessOperation::kExecute),
                AccessAction::kPermit, "rpc"});
  RecordingOperationProvider operations;
  NetconfServer server(stores, &nacm, nullptr, nullptr, std::nullopt, nullptr,
                       &operations);
  const std::string request = R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="action">
      <action xmlns="urn:ietf:params:xml:ns:yang:1">
        <system xmlns="urn:rpc-test"><reset/></system>
      </action>
    </rpc>)xml";
  EXPECT_NE(server.Process("alice", request).xml.find("access-denied"),
            std::string::npos);
  EXPECT_TRUE(operations.called.empty());

  nacm.AddRule({"system", "operators", "", "",
                AccessMask(AccessOperation::kRead), AccessAction::kPermit,
                "rpc"});
  const RpcResponse allowed = server.Process("alice", request);
  EXPECT_EQ(operations.called, "rpc:reset");
  EXPECT_EQ(operations.path,
            "/{urn:rpc-test}system/{urn:rpc-test}reset");
  EXPECT_NE(allowed.xml.find("reset"), std::string::npos) << allowed.xml;
}

TEST(NetconfServerTest, RequiresActionParentInstanceAndCompleteListKeys) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy nacm;
  nacm.AddUserToGroup("alice", "operators");
  nacm.set_read_default(AccessAction::kDeny);
  nacm.set_exec_default(AccessAction::kDeny);
  nacm.AddRule({"read", "operators", "", "",
                AccessMask(AccessOperation::kRead), AccessAction::kPermit,
                "rpc"});
  nacm.AddRule({"actions", "operators", "", "",
                AccessMask(AccessOperation::kExecute), AccessAction::kPermit,
                "rpc"});
  RecordingOperationProvider operations;
  NetconfServer server(stores, &nacm, nullptr, nullptr, std::nullopt, nullptr,
                       &operations);
  const auto request = [](std::string_view body) {
    return "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
           "message-id=\"action-instance\"><action "
           "xmlns=\"urn:ietf:params:xml:ns:yang:1\">" +
           std::string(body) + "</action></rpc>";
  };

  const RpcResponse hidden = server.Process("bob", request(R"xml(
    <device xmlns="urn:rpc-test"><name>edge-2</name><bounce/></device>)xml"));
  EXPECT_NE(hidden.xml.find("access-denied"), std::string::npos) << hidden.xml;
  EXPECT_EQ(hidden.xml.find("data-missing"), std::string::npos) << hidden.xml;
  EXPECT_TRUE(operations.called.empty());

  const RpcResponse existing = server.Process("alice", request(R"xml(
    <device xmlns="urn:rpc-test"><name>edge-1</name><bounce/></device>)xml"));
  EXPECT_EQ(operations.called, "rpc:bounce");
  EXPECT_NE(existing.xml.find("reset"), std::string::npos) << existing.xml;
  EXPECT_NE(operations.path.find("name='edge-1'"), std::string::npos)
      << operations.path;

  operations.called.clear();
  const RpcResponse absent = server.Process("alice", request(R"xml(
    <device xmlns="urn:rpc-test"><name>edge-2</name><bounce/></device>)xml"));
  EXPECT_NE(absent.xml.find("data-missing"), std::string::npos) << absent.xml;
  EXPECT_TRUE(operations.called.empty());

  const RpcResponse missing_key = server.Process("alice", request(R"xml(
    <device xmlns="urn:rpc-test"><bounce/></device>)xml"));
  EXPECT_NE(missing_key.xml.find("missing-element"), std::string::npos)
      << missing_key.xml;
  EXPECT_TRUE(operations.called.empty());
}

TEST(NetconfServerTest, AppliesXPathRetrievalFilter) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  const RpcResponse selected = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="xpath">
      <get><filter type="xpath" xmlns:r="urn:rpc-test"
                   select="/r:system/r:hostname"/></get>
    </rpc>)xml");
  EXPECT_NE(selected.xml.find("<r:hostname>old</r:hostname>"),
            std::string::npos) << selected.xml;
  const RpcResponse invalid = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="bad-xpath">
      <get><filter type="xpath" select="count(/*)"/></get>
    </rpc>)xml");
  EXPECT_NE(invalid.xml.find("invalid-value"), std::string::npos);
}

TEST(NetconfServerTest, DispatchesEditCommitAndGetConfig) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  const RpcResponse edited = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="101">
      <edit-config><target><candidate/></target><config>
        <system xmlns="urn:rpc-test"><hostname>new</hostname></system>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(edited.xml.find("message-id=\"101\""), std::string::npos);
  EXPECT_NE(edited.xml.find("<ok/>"), std::string::npos) << edited.xml;
  EXPECT_NE(server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="102">
      <commit/>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);
  const RpcResponse read = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="103">
      <get-config><source><running/></source></get-config>
    </rpc>)xml");
  EXPECT_NE(read.xml.find("<data>"), std::string::npos);
  EXPECT_NE(read.xml.find(">new</"), std::string::npos);
}

TEST(NetconfServerTest, SerializesErrorsAndClosesSession) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  const RpcResponse unsupported = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="bad&amp;id">
      <unknown/>
    </rpc>)xml");
  EXPECT_NE(unsupported.xml.find("message-id=\"bad&amp;id\""),
            std::string::npos);
  EXPECT_NE(unsupported.xml.find("operation-not-supported"), std::string::npos);
  const RpcResponse close = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="104">
      <close-session/>
    </rpc>)xml");
  EXPECT_TRUE(close.close_session);
  EXPECT_NE(close.xml.find("<ok/>"), std::string::npos);
}

TEST(NetconfServerTest, RejectsMalformedRpcAndInvalidOptions) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  EXPECT_NE(server.Process("17", "<rpc>").xml.find("malformed-message"),
            std::string::npos);
  const RpcResponse invalid = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="105">
      <edit-config><target><candidate/></target>
        <default-operation>bogus</default-operation><config/>
      </edit-config>
    </rpc>)xml");
  EXPECT_NE(invalid.xml.find("invalid-value"), std::string::npos);
  const RpcResponse wrong_namespace = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="106">
      <get xmlns="urn:not-netconf"/>
    </rpc>)xml");
  EXPECT_NE(wrong_namespace.xml.find("unknown-namespace"), std::string::npos);
  const RpcResponse filtered = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="107">
      <get><filter/></get>
    </rpc>)xml");
  EXPECT_NE(filtered.xml.find("<data"), std::string::npos);
  EXPECT_EQ(filtered.xml.find("<system"), std::string::npos);
}

TEST(NetconfServerTest, ReplaysRfc6241InteroperabilityVectors) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  const std::filesystem::path path =
      std::filesystem::path(YANG_TEST_SOURCE_DIR) / "fixtures" / "netconf" /
      "rfc6241-vectors.json";
  std::ifstream input(path);
  ASSERT_TRUE(input);
  const nlohmann::json corpus = nlohmann::json::parse(input);
  for (const auto& vector : corpus.at("vectors")) {
    SCOPED_TRACE(vector.at("name").get<std::string>());
    const RpcResponse response = server.Process(
        "interop-session", vector.at("request").get<std::string>());
    for (const auto& expected : vector.at("expected")) {
      EXPECT_NE(response.xml.find(expected.get<std::string>()), std::string::npos)
          << response.xml;
    }
  }
}

TEST(NetconfServerTest, ContinuesPersistentConfirmedCommitSequence) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  ASSERT_NE(server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="201">
      <edit-config><target><candidate/></target><config>
        <system xmlns="urn:rpc-test"><hostname>first</hostname></system>
      </config></edit-config>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);
  ASSERT_NE(server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="202">
      <commit><confirmed/><persist>token</persist></commit>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);
  ASSERT_NE(server.Process("18", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="203">
      <edit-config><target><candidate/></target><config>
        <system xmlns="urn:rpc-test"><hostname>second</hostname></system>
      </config></edit-config>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);
  EXPECT_NE(server.Process("18", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="204">
      <commit><confirmed/><persist-id>token</persist-id></commit>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);
  EXPECT_NE(server.Process("19", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="205">
      <commit><persist-id>token</persist-id></commit>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find(">second</"),
            std::string::npos);
}

TEST(NetconfServerTest, CopiesCompleteInlineConfigurationAtomically) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  const RpcResponse copied = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="copy">
      <copy-config><target><candidate/></target><source><config>
        <system xmlns="urn:rpc-test"><hostname>inline</hostname></system>
      </config></source></copy-config>
    </rpc>)xml");
  EXPECT_NE(copied.xml.find("<ok/>"), std::string::npos) << copied.xml;
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find(">inline</"),
            std::string::npos);
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml().find("device"),
            std::string::npos);

  const RpcResponse invalid = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="invalid">
      <copy-config><target><candidate/></target><source><config>
        <system xmlns="urn:rpc-test"/>
      </config></source></copy-config>
    </rpc>)xml");
  EXPECT_NE(invalid.xml.find("rpc-error"), std::string::npos);
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find(">inline</"),
            std::string::npos);
}

TEST(NetconfServerTest, RejectsCopyToSameDatastore) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NetconfServer server(stores);
  const RpcResponse response = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="same">
      <copy-config><target><running/></target><source><running/></source>
      </copy-config>
    </rpc>)xml");
  EXPECT_NE(response.xml.find("invalid-value"), std::string::npos);
}

TEST(NetconfServerTest, AppliesNacmCopyConfigSourceAndStartupSpecialCase) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy policy;
  policy.set_read_default(AccessAction::kPermit);
  policy.set_write_default(AccessAction::kPermit);
  policy.AddUserToGroup("alice", "users");
  policy.AddRule({"hide-device", "users", "",
                  "/{urn:rpc-test}device",
                  AccessMask(AccessOperation::kRead), AccessAction::kDeny});
  EXPECT_FALSE(policy.AuthorizeData(
      "alice", "rpc", AccessOperation::kRead,
      "/{urn:rpc-test}device[{urn:rpc-test}name='edge-1']"));
  const std::string directly_filtered = policy.FilterReadableData(
      "alice",
      "<data xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">" +
          stores.Read(Datastore::kRunning).ToXml(false) + "</data>",
      {}, &fixture->schema);
  EXPECT_EQ(directly_filtered.find("device"), std::string::npos)
      << directly_filtered;
  NetconfServer server(stores, &policy);
  const RpcResponse filtered = server.Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="filtered">
      <copy-config><target><candidate/></target><source><running/></source>
      </copy-config>
    </rpc>)xml");
  EXPECT_NE(filtered.xml.find("<ok/>"), std::string::npos) << filtered.xml;
  const std::string candidate = stores.Read(Datastore::kCandidate).ToXml();
  EXPECT_EQ(candidate.find("device"), std::string::npos) << candidate;

  policy.set_read_default(AccessAction::kDeny);
  policy.set_write_default(AccessAction::kDeny);
  const RpcResponse startup = server.Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="startup">
      <copy-config><target><startup/></target><source><running/></source>
      </copy-config>
    </rpc>)xml");
  EXPECT_NE(startup.xml.find("<ok/>"), std::string::npos) << startup.xml;
  EXPECT_EQ(stores.Read(Datastore::kStartup).ToXml(),
            stores.Read(Datastore::kRunning).ToXml());
}

TEST(NetconfServerTest, UsesHostSuppliedUrlDatastores) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  MemoryUrlProvider urls;
  urls.values["memory:incoming"] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:rpc-test"><hostname>remote</hostname></system>
    </config>)xml";
  NetconfServer server(stores, nullptr, &urls);
  EXPECT_NE(server.ServerHello(1).find("capability:url:1.0?scheme=memory"),
            std::string::npos);
  EXPECT_NE(server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="read-url">
      <copy-config><target><candidate/></target>
        <source><url>memory:incoming</url></source></copy-config></rpc>)xml")
                .xml.find("<ok/>"), std::string::npos);
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find("remote"),
            std::string::npos);
  EXPECT_NE(server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="write-url">
      <copy-config><target><url>memory:out</url></target>
        <source><running/></source></copy-config></rpc>)xml")
                .xml.find("<ok/>"), std::string::npos);
  EXPECT_TRUE(urls.values.contains("memory:out"));

  urls.values["memory:edit"] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:rpc-test"><hostname>edited-url</hostname></system>
    </config>)xml";
  EXPECT_NE(server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="edit-url">
      <edit-config><target><candidate/></target><url>memory:edit</url>
      </edit-config></rpc>)xml").xml.find("<ok/>"), std::string::npos);
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find("edited-url"),
            std::string::npos);
  EXPECT_NE(server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="validate-url">
      <validate><source><url>memory:edit</url></source></validate></rpc>)xml")
                .xml.find("<ok/>"), std::string::npos);
  EXPECT_NE(server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="same-url">
      <copy-config><target><url>memory:edit</url></target>
        <source><url>memory:edit</url></source></copy-config></rpc>)xml")
                .xml.find("invalid-value"), std::string::npos);
  urls.values["memory:invalid"] = "<config/>";
  const RpcResponse invalid_remote = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="invalid-remote">
      <copy-config><target><url>memory:must-not-write</url></target>
        <source><url>memory:invalid</url></source></copy-config></rpc>)xml");
  EXPECT_NE(invalid_remote.xml.find("rpc-error"), std::string::npos);
  EXPECT_FALSE(urls.values.contains("memory:must-not-write"));
  EXPECT_NE(server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="missing-url">
      <validate><source><url>memory:missing</url></source></validate></rpc>)xml")
                .xml.find("operation-failed"), std::string::npos);
  EXPECT_NE(server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="bad-scheme">
      <validate><source><url>file:/forbidden</url></source></validate></rpc>)xml")
                .xml.find("invalid-value"), std::string::npos);
}

TEST(NetconfServerTest, EnforcesNacmBeforePublishingWrites) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy policy;
  policy.AddUserToGroup("guest", "guests");
  NetconfServer server(stores, &policy);
  const RpcResponse denied = server.Process("guest", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="301">
      <edit-config><target><candidate/></target><config>
        <system xmlns="urn:rpc-test"><hostname>forbidden</hostname></system>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(denied.xml.find("access-denied"), std::string::npos);
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml().find("forbidden"),
            std::string::npos);
}

TEST(NetconfServerTest, NacmRpcDenialIdentifiesNetconfOperation) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy policy;
  policy.set_exec_default(AccessAction::kDeny);
  NetconfServer server(stores, &policy);
  const RpcResponse denied = server.Process("guest", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="deny">
      <edit-config><target><candidate/></target><config/></edit-config>
    </rpc>)xml");
  EXPECT_NE(denied.xml.find("<error-type>application</error-type>"),
            std::string::npos) << denied.xml;
  EXPECT_NE(denied.xml.find("<error-tag>access-denied</error-tag>"),
            std::string::npos) << denied.xml;
  EXPECT_NE(denied.xml.find(
                "xmlns:nc=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
                "/nc:rpc/nc:edit-config</error-path>"),
            std::string::npos) << denied.xml;
  EXPECT_EQ(denied.xml.find("<error-info>"), std::string::npos) << denied.xml;
}

TEST(NetconfServerTest, AuthorizesUrlTargetReplacementBeforeWriting) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  MemoryUrlProvider urls;
  urls.values["memory:protected"] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:rpc-test"><hostname>remote-old</hostname></system>
    </config>)xml";
  NacmPolicy policy;
  policy.set_write_default(AccessAction::kPermit);
  policy.AddUserToGroup("guest", "guests");
  policy.AddRule({"deny-hostname", "guests", "",
                  "/{urn:rpc-test}system/{urn:rpc-test}hostname",
                  static_cast<std::uint8_t>(
                      AccessMask(AccessOperation::kCreate) |
                      AccessMask(AccessOperation::kUpdate) |
                      AccessMask(AccessOperation::kDelete)),
                  AccessAction::kDeny});
  NetconfServer server(stores, &policy, &urls);
  const RpcResponse denied = server.Process("guest", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="url-nacm">
      <copy-config><target><url>memory:protected</url></target>
        <source><running/></source></copy-config>
    </rpc>)xml");
  EXPECT_NE(denied.xml.find("access-denied"), std::string::npos) << denied.xml;
  EXPECT_NE(urls.values["memory:protected"].find("remote-old"),
            std::string::npos);
}

}  // namespace
}  // namespace yang::netconf
