// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
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
    ++read_calls[std::string(url)];
    if (const auto failed = read_failures.find(std::string(url));
        failed != read_failures.end()) {
      return failed->second;
    }
    const auto found = values.find(std::string(url));
    return found == values.end() ? UrlResult{{}, "not found"}
                                 : UrlResult{found->second, {}};
  }
  UrlResult Write(std::string_view url, std::string_view xml) override {
    ++write_calls[std::string(url)];
    if (const auto failed = write_failures.find(std::string(url));
        failed != write_failures.end()) {
      return failed->second;
    }
    values[std::string(url)] = std::string(xml);
    return {};
  }
  UrlResult Delete(std::string_view url) override {
    values.erase(std::string(url));
    return {};
  }
  std::map<std::string, std::string> values;
  std::map<std::string, UrlResult> read_failures;
  std::map<std::string, UrlResult> write_failures;
  std::map<std::string, std::size_t> read_calls;
  std::map<std::string, std::size_t> write_calls;
};

class RecordingOperationProvider final : public OperationProvider {
 public:
  OperationResult InvokeRpc(const RpcSessionContext&,
      const config::RuntimeSchemaNode& operation,
      std::string_view operation_xml) override {
    called = operation.module_name + ":" + operation.name.local_name;
    input = operation_xml;
    return {{true, {}, {}}, rpc_output};
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
  std::string rpc_output =
      R"xml(<result xmlns="urn:rpc-test">pong</result>)xml";
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

class StatelessOperationProvider final : public OperationProvider {
 public:
  OperationResult InvokeRpc(const RpcSessionContext&,
      const config::RuntimeSchemaNode&, std::string_view) override {
    return {{true, {}, {}},
            R"xml(<result xmlns="urn:rpc-test">concurrent</result>)xml"};
  }
};

class MinimalOperationalProvider final : public OperationalDataProvider {
 public:
  DataResult AugmentDataXml(
      std::string_view configuration_data_xml) const override {
    return {std::string(configuration_data_xml), {}};
  }
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

TEST(NetconfServerTest, AuditsEveryRecoveryUserRpcAttempt) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy policy;
  policy.set_exec_default(AccessAction::kDeny);
  ASSERT_TRUE(policy.AddRecoveryUser("rescue"));
  NetconfServer server(stores, &policy);
  std::mutex records_mutex;
  std::vector<RecoveryAuditRecord> records;
  server.SetRecoveryAuditSink([&](const RecoveryAuditRecord& record) {
    std::lock_guard lock(records_mutex);
    records.push_back(record);
  });
  const std::string get =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"audit\"><get/></rpc>";

  const RpcResponse recovery =
      server.Process({401, "rescue", "rescue", {}}, get);
  EXPECT_EQ(recovery.xml.find("access-denied"), std::string::npos)
      << recovery.xml;
  (void)server.Process({402, "ordinary", "ordinary", {}}, get);
  (void)server.Process({403, "rescue", "rescue", {}}, "not XML");

  std::vector<std::thread> sessions;
  for (std::uint32_t id = 410; id < 418; ++id) {
    sessions.emplace_back([&, id] {
      (void)server.Process({id, "rescue", "rescue", {}}, get);
    });
  }
  for (std::thread& session : sessions) session.join();

  std::lock_guard lock(records_mutex);
  ASSERT_EQ(records.size(), 10U);
  EXPECT_EQ(records.front().session_id, 401U);
  EXPECT_EQ(records.front().username, "rescue");
  EXPECT_EQ(records.front().rpc_bytes, get.size());
  EXPECT_EQ(records[1].session_id, 403U);
  EXPECT_EQ(records[1].rpc_bytes, 7U);
  EXPECT_TRUE(std::ranges::all_of(records, [](const RecoveryAuditRecord& record) {
    return record.username == "rescue" && record.session_id != 402;
  }));

  server.SetRecoveryAuditSink([](const RecoveryAuditRecord&) {
    throw std::runtime_error("audit unavailable");
  });
  const RpcResponse audit_failed =
      server.Process({404, "rescue", "rescue", {}}, get);
  EXPECT_NE(audit_failed.xml.find("operation-failed"), std::string::npos)
      << audit_failed.xml;
  EXPECT_NE(audit_failed.xml.find("recovery audit sink failed"),
            std::string::npos) << audit_failed.xml;
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

  operations.rpc_output =
      "<!DOCTYPE result [<!ENTITY x 'smuggled'>]>"
      "<result xmlns='urn:rpc-test'>&x;</result>";
  const RpcResponse malicious_output = denied.Process("alice", request);
  EXPECT_NE(malicious_output.xml.find("operation-failed"), std::string::npos)
      << malicious_output.xml;
  EXPECT_EQ(malicious_output.xml.find("smuggled"), std::string::npos)
      << malicious_output.xml;
  operations.rpc_output =
      R"xml(<result xmlns="urn:rpc-test">pong</result>)xml";

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

TEST(NetconfServerTest, ReplacesPolicySafelyAcrossConcurrentSessions) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy policy;
  policy.set_exec_default(AccessAction::kPermit);
  policy.set_read_default(AccessAction::kDeny);
  StatelessOperationProvider operations;
  NetconfServer server(stores, &policy, nullptr, nullptr, std::nullopt, nullptr,
                       &operations);
  const std::string request = R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="race">
      <ping xmlns="urn:rpc-test"><count>7</count></ping>
    </rpc>)xml";

  NacmPolicy deny_execute;
  deny_execute.set_exec_default(AccessAction::kDeny);
  deny_execute.set_read_default(AccessAction::kPermit);
  deny_execute.PreserveRuntimeStateFrom(policy);
  NacmPolicy permit_execute = policy;
  std::atomic<bool> start = false;
  std::atomic<unsigned> leaked_outputs = 0;
  std::vector<std::thread> sessions;
  for (unsigned session = 0; session < 4; ++session) {
    sessions.emplace_back([&, session] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      const std::string username = "user-" + std::to_string(session);
      for (unsigned request_number = 0; request_number < 250;
           ++request_number) {
        const RpcSessionContext context{session + 1, username, username, {}};
        const RpcResponse response = server.Process(context, request);
        // Each complete policy either denies execution or filters the output.
        // A visible result would combine fields from two policy generations.
        if (response.xml.find("<result") != std::string::npos)
          leaked_outputs.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::thread replacement([&] {
    start.store(true, std::memory_order_release);
    for (unsigned generation = 0; generation < 500; ++generation) {
      policy = generation % 2 == 0 ? deny_execute : permit_execute;
      std::this_thread::yield();
    }
  });
  replacement.join();
  for (std::thread& session : sessions) session.join();

  EXPECT_EQ(leaked_outputs.load(), 0U);
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
  const RpcResponse multiple_roots = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="smuggle">
      <close-session/>
    </rpc>
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="hidden">
      <get/>
    </rpc>)xml");
  EXPECT_NE(multiple_roots.xml.find("malformed-message"), std::string::npos);
  EXPECT_FALSE(multiple_roots.close_session);
  std::string embedded_nul =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"nul\"><get/>";
  embedded_nul.push_back('\0');
  embedded_nul += "<close-session/></rpc>";
  EXPECT_NE(server.Process("17", embedded_nul).xml.find("malformed-message"),
            std::string::npos);
  const RpcResponse entity = server.Process("17", R"xml(
    <!DOCTYPE rpc [<!ENTITY hidden "<close-session/>">]>
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="entity">
      &hidden;
    </rpc>)xml");
  EXPECT_NE(entity.xml.find("malformed-message"), std::string::npos);
  EXPECT_FALSE(entity.close_session);
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

TEST(NetconfServerTest, DoesNotApplySessionNacmToConfirmedCommitRollback) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy policy;
  policy.set_write_default(AccessAction::kPermit);
  NetconfServer server(stores, &policy);
  const auto edit_and_confirm = [&](std::string_view value,
                                    std::string_view message_id) {
    const std::string edit =
        "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
        "message-id=\"" + std::string(message_id) +
        "\"><edit-config><target><candidate/></target><config>"
        "<system xmlns=\"urn:rpc-test\"><hostname>" + std::string(value) +
        "</hostname></system></config></edit-config></rpc>";
    EXPECT_NE(server.Process("alice", edit).xml.find("<ok/>"),
              std::string::npos);
    const RpcResponse committed = server.Process("alice", R"xml(
      <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="commit">
        <commit><confirmed/><confirm-timeout>60</confirm-timeout></commit>
      </rpc>)xml");
    EXPECT_NE(committed.xml.find("<ok/>"), std::string::npos) << committed.xml;
  };

  edit_and_confirm("temporary", "edit-cancel");
  ASSERT_NE(stores.Read(Datastore::kRunning).ToXml().find("temporary"),
            std::string::npos);
  policy.set_write_default(AccessAction::kDeny);
  const RpcResponse cancelled = server.Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="cancel">
      <cancel-commit/>
    </rpc>)xml");
  EXPECT_NE(cancelled.xml.find("<ok/>"), std::string::npos) << cancelled.xml;
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find(">old</"),
            std::string::npos);

  policy.set_write_default(AccessAction::kPermit);
  edit_and_confirm("expires", "edit-timeout");
  policy.set_write_default(AccessAction::kDeny);
  EXPECT_TRUE(stores.ProcessTimeouts(DatastoreManager::Clock::now() +
                                     std::chrono::hours(1)));
  EXPECT_NE(stores.Read(Datastore::kRunning).ToXml().find(">old</"),
            std::string::npos);
  EXPECT_EQ(policy.counters().denied_data_writes, 0U);
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

TEST(NetconfServerTest, OmitsUnmodeledSchemaAwareNacmReadData) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  NacmPolicy policy;
  policy.set_read_default(AccessAction::kPermit);
  const std::string data = R"xml(
    <data xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:rpc-test"><hostname>visible</hostname>
        <injected xmlns="urn:unknown">secret</injected>
      </system>
      <device xmlns="urn:rpc-test"><name>edge-ok</name></device>
      <device xmlns="urn:rpc-test"><name>one</name><name>two</name></device>
      <device xmlns="urn:rpc-test"><name>both'&quot;quotes</name></device>
      <device xmlns="urn:rpc-test"/>
      <foreign xmlns="urn:unknown">secret</foreign>
    </data>)xml";

  const std::string filtered = policy.FilterReadableData(
      "alice", data, {}, &fixture->schema);
  EXPECT_NE(filtered.find("<hostname>visible</hostname>"), std::string::npos);
  EXPECT_EQ(filtered.find("injected"), std::string::npos);
  EXPECT_EQ(filtered.find("foreign"), std::string::npos);
  EXPECT_NE(filtered.find("<name>edge-ok</name>"), std::string::npos);
  EXPECT_EQ(filtered.find("<name>one</name>"), std::string::npos);
  EXPECT_EQ(filtered.find("<name>two</name>"), std::string::npos);
  EXPECT_EQ(filtered.find("both'\"quotes"), std::string::npos);
  const std::size_t first_device = filtered.find("<device");
  ASSERT_NE(first_device, std::string::npos);
  EXPECT_EQ(filtered.find("<device", first_device + 1), std::string::npos);

  NacmPolicy recovery;
  ASSERT_TRUE(recovery.AddRecoveryUser("root"));
  const std::string recovery_filtered = recovery.FilterReadableData(
      "root", data, {}, &fixture->schema);
  EXPECT_NE(recovery_filtered.find("<hostname>visible</hostname>"),
            std::string::npos);
  EXPECT_EQ(recovery_filtered.find("injected"), std::string::npos);
  EXPECT_EQ(recovery_filtered.find("foreign"), std::string::npos);
  EXPECT_NE(recovery_filtered.find("<name>edge-ok</name>"), std::string::npos);
  EXPECT_EQ(recovery_filtered.find("<name>one</name>"), std::string::npos);

  NacmPolicy disabled;
  disabled.set_enabled(false);
  const std::string disabled_filtered = disabled.FilterReadableData(
      "alice", data, {}, &fixture->schema);
  EXPECT_NE(disabled_filtered.find("<hostname>visible</hostname>"),
            std::string::npos);
  EXPECT_EQ(disabled_filtered.find("injected"), std::string::npos);
  EXPECT_EQ(disabled_filtered.find("foreign"), std::string::npos);
  EXPECT_NE(disabled_filtered.find("<name>edge-ok</name>"), std::string::npos);
  EXPECT_EQ(disabled_filtered.find("<name>one</name>"), std::string::npos);

  // Without a schema, retain the generic XML filtering API's established
  // behavior; the caller did not supply a model as an authority.
  const std::string generic = policy.FilterReadableData("alice", data);
  EXPECT_NE(generic.find("injected"), std::string::npos);
  EXPECT_NE(generic.find("foreign"), std::string::npos);
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
  urls.values["memory:entity"] =
      "<!DOCTYPE config [<!ENTITY host 'smuggled'>]>"
      "<config xmlns='urn:ietf:params:xml:ns:netconf:base:1.0'>"
      "<system xmlns='urn:rpc-test'><hostname>&host;</hostname></system>"
      "</config>";
  const RpcResponse entity_remote = server.Process("17", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="entity-url">
      <edit-config><target><candidate/></target><url>memory:entity</url>
      </edit-config></rpc>)xml");
  EXPECT_NE(entity_remote.xml.find("invalid-value"), std::string::npos)
      << entity_remote.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml().find("smuggled"),
            std::string::npos);
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
  policy.AddRule({"classified-rule-name", "guests", "",
                  "/{urn:rpc-test}system/{urn:rpc-test}hostname",
                  AccessMask(AccessOperation::kUpdate), AccessAction::kDeny});
  NetconfServer server(stores, &policy);
  const RpcResponse denied = server.Process("guest", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="301">
      <edit-config><target><candidate/></target><config>
        <system xmlns="urn:rpc-test"><hostname>forbidden</hostname></system>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(denied.xml.find("access-denied"), std::string::npos);
  EXPECT_NE(denied.xml.find(
                "xmlns:n0=\"urn:rpc-test\">/n0:system/n0:hostname"
                "</error-path>"),
            std::string::npos) << denied.xml;
  EXPECT_EQ(denied.xml.find("<error-path>/{urn:"), std::string::npos)
      << denied.xml;
  EXPECT_EQ(denied.xml.find("{urn:"), std::string::npos) << denied.xml;
  EXPECT_EQ(denied.xml.find("forbidden"), std::string::npos) << denied.xml;
  EXPECT_EQ(denied.xml.find("classified-rule-name"), std::string::npos)
      << denied.xml;
  EXPECT_NE(denied.xml.find(
                "<error-message xml:lang=\"en\">access to the proposed "
                "datastore change is denied</error-message>"),
            std::string::npos) << denied.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml().find("forbidden"),
            std::string::npos);

  const RpcResponse keyed = server.Process("guest", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="302">
      <edit-config><target><candidate/></target><config>
        <device xmlns="urn:rpc-test"><name>edge-2</name></device>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(keyed.xml.find(
                "/n0:device[n0:name=&apos;edge-2&apos;]</error-path>"),
            std::string::npos) << keyed.xml;
}

TEST(NetconfServerTest, MapsEffectiveEditConfigChangesToCrudBits) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy policy;
  policy.set_write_default(AccessAction::kDeny);
  for (const std::string& user : {"creator", "updater", "deleter"})
    policy.AddUserToGroup(user, user);
  policy.AddRule({"create-device", "creator", "",
                  "/{urn:rpc-test}device",
                  AccessMask(AccessOperation::kCreate), AccessAction::kPermit});
  policy.AddRule({"update-hostname", "updater", "",
                  "/{urn:rpc-test}system/{urn:rpc-test}hostname",
                  AccessMask(AccessOperation::kUpdate), AccessAction::kPermit});
  policy.AddRule({"delete-device", "deleter", "",
                  "/{urn:rpc-test}device",
                  AccessMask(AccessOperation::kDelete), AccessAction::kPermit});
  NetconfServer server(stores, &policy);

  const RpcResponse created = server.Process("creator", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="create">
      <edit-config><target><candidate/></target><config>
        <device xmlns="urn:rpc-test"><name>edge-2</name></device>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(created.xml.find("<ok/>"), std::string::npos) << created.xml;
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find("edge-2"),
            std::string::npos);

  const RpcResponse updated = server.Process("updater", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="update">
      <edit-config><target><candidate/></target><config>
        <system xmlns="urn:rpc-test"><hostname>new</hostname></system>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(updated.xml.find("<ok/>"), std::string::npos) << updated.xml;
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find(">new</"),
            std::string::npos);

  const RpcResponse deleted = server.Process("deleter", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="delete">
      <edit-config><target><candidate/></target><config>
        <device xmlns="urn:rpc-test"
          xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
          nc:operation="delete"><name>edge-1</name></device>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(deleted.xml.find("<ok/>"), std::string::npos) << deleted.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml().find("edge-1"),
            std::string::npos);

  const std::string before = stores.Read(Datastore::kCandidate).ToXml();
  const RpcResponse wrong_bit = server.Process("updater", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="wrong-bit">
      <edit-config><target><candidate/></target><config>
        <device xmlns="urn:rpc-test"><name>edge-3</name></device>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(wrong_bit.xml.find("access-denied"), std::string::npos)
      << wrong_bit.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml(), before);
}

TEST(NetconfServerTest, MapsEffectiveEditDataChangesToCrudBits) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy policy;
  policy.set_write_default(AccessAction::kDeny);
  for (const std::string& user : {"creator", "updater", "deleter"})
    policy.AddUserToGroup(user, user);
  policy.AddRule({"create-device", "creator", "",
                  "/{urn:rpc-test}device",
                  AccessMask(AccessOperation::kCreate), AccessAction::kPermit});
  policy.AddRule({"update-hostname", "updater", "",
                  "/{urn:rpc-test}system/{urn:rpc-test}hostname",
                  AccessMask(AccessOperation::kUpdate), AccessAction::kPermit});
  policy.AddRule({"delete-device", "deleter", "",
                  "/{urn:rpc-test}device",
                  AccessMask(AccessOperation::kDelete), AccessAction::kPermit});
  NetconfServer server(stores, &policy);

  const RpcResponse created = server.Process("creator", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="create"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <edit-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:candidate</datastore><config>
          <device xmlns="urn:rpc-test"><name>edge-2</name></device>
        </config>
      </edit-data>
    </rpc>)xml");
  EXPECT_NE(created.xml.find("<ok/>"), std::string::npos) << created.xml;
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find("edge-2"),
            std::string::npos);

  const RpcResponse updated = server.Process("updater", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="update"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <edit-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:candidate</datastore><config>
          <system xmlns="urn:rpc-test"><hostname>new</hostname></system>
        </config>
      </edit-data>
    </rpc>)xml");
  EXPECT_NE(updated.xml.find("<ok/>"), std::string::npos) << updated.xml;
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find(">new</"),
            std::string::npos);

  const RpcResponse deleted = server.Process("deleter", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="delete"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <edit-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:candidate</datastore><config>
          <device xmlns="urn:rpc-test"
            xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
            nc:operation="delete"><name>edge-1</name></device>
        </config>
      </edit-data>
    </rpc>)xml");
  EXPECT_NE(deleted.xml.find("<ok/>"), std::string::npos) << deleted.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml().find("edge-1"),
            std::string::npos);

  const std::string before = stores.Read(Datastore::kCandidate).ToXml();
  const RpcResponse wrong_bit = server.Process("updater", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="wrong-bit"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <edit-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:candidate</datastore><config>
          <device xmlns="urn:rpc-test"><name>edge-3</name></device>
        </config>
      </edit-data>
    </rpc>)xml");
  EXPECT_NE(wrong_bit.xml.find("access-denied"), std::string::npos)
      << wrong_bit.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml(), before);
}

TEST(NetconfServerTest, RequiresNacmUpdatePermissionForOrderedMove) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("ordered.yang", R"yang(module ordered {
    yang-version 1.1; namespace "urn:ordered-test"; prefix o;
    list item { key name; ordered-by user; leaf name { type string; } }
  })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  ASSERT_TRUE(compilation);
  config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto initial = config::ParseDatastoreXml(schema, R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <item xmlns="urn:ordered-test"><name>a</name></item>
      <item xmlns="urn:ordered-test"><name>b</name></item>
      <item xmlns="urn:ordered-test"><name>c</name></item>
    </config>)xml").document;
  ASSERT_TRUE(initial);
  DatastoreManager stores(schema, *initial);
  NacmPolicy policy;
  policy.set_write_default(AccessAction::kDeny);
  policy.AddUserToGroup("mover", "movers");
  policy.AddUserToGroup("creator", "creators");
  policy.AddRule({"move-item", "movers", "", "/{urn:ordered-test}item",
                  AccessMask(AccessOperation::kUpdate), AccessAction::kPermit});
  policy.AddRule({"create-item", "creators", "", "/{urn:ordered-test}item",
                  AccessMask(AccessOperation::kCreate), AccessAction::kPermit});
  NetconfServer server(stores, &policy);

  const RpcResponse moved = server.Process("mover", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="move">
      <edit-config><target><candidate/></target><config>
        <item xmlns="urn:ordered-test" xmlns:o="urn:ordered-test"
              xmlns:y="urn:ietf:params:xml:ns:yang:1"
              y:insert="after" y:key="[o:name='c']"><name>a</name></item>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(moved.xml.find("<ok/>"), std::string::npos) << moved.xml;
  const std::string reordered = stores.Read(Datastore::kCandidate).ToXml(false);
  EXPECT_LT(reordered.find(">b</"), reordered.find(">c</"));
  EXPECT_LT(reordered.find(">c</"), reordered.find(">a</"));

  const RpcResponse denied = server.Process("creator", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="deny-move">
      <edit-config><target><candidate/></target><config>
        <item xmlns="urn:ordered-test"
              xmlns:y="urn:ietf:params:xml:ns:yang:1"
              y:insert="first"><name>a</name></item>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(denied.xml.find("access-denied"), std::string::npos) << denied.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml(false), reordered);
}

TEST(NetconfServerTest, AuthorizesSpecificLeafListInstances) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("leaf-list.yang", R"yang(module leaf-list {
    yang-version 1.1; namespace "urn:leaf-list-test"; prefix ll;
    container preferences { leaf-list color { type string; } }
  })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  ASSERT_TRUE(compilation);
  config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto initial = config::ParseDatastoreXml(schema, R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <preferences xmlns="urn:leaf-list-test">
        <color>red</color><color>black</color><color>user's choice</color>
        <color>user's &quot;choice</color>
      </preferences>
    </config>)xml").document;
  ASSERT_TRUE(initial);
  DatastoreManager stores(schema, *initial);
  NacmPolicy policy;
  policy.set_write_default(AccessAction::kDeny);
  for (const std::string& user : {"creator", "deleter", "viewer"})
    policy.AddUserToGroup(user, user);
  const std::string colors =
      "/{urn:leaf-list-test}preferences/{urn:leaf-list-test}color";
  policy.AddRule({"create-blue", "creator", "", colors + "[.='blue']",
                  AccessMask(AccessOperation::kCreate), AccessAction::kPermit});
  policy.AddRule({"delete-blue", "deleter", "", colors + "[.='blue']",
                  AccessMask(AccessOperation::kDelete), AccessAction::kPermit});
  policy.AddRule({"hide-red", "viewer", "", colors + "[.='red']",
                  AccessMask(AccessOperation::kRead), AccessAction::kDeny});
  policy.AddRule({"hide-apostrophe", "viewer", "",
                  colors + "[.=\"user's choice\"]",
                  AccessMask(AccessOperation::kRead), AccessAction::kDeny});
  NetconfServer server(stores, &policy);

  const RpcResponse filtered = server.Process("viewer", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="read">
      <get-config><source><candidate/></source></get-config>
    </rpc>)xml");
  EXPECT_EQ(filtered.xml.find(">red</"), std::string::npos) << filtered.xml;
  EXPECT_EQ(filtered.xml.find("user's choice"), std::string::npos)
      << filtered.xml;
  EXPECT_EQ(filtered.xml.find("user's \"choice"), std::string::npos)
      << filtered.xml;
  EXPECT_NE(filtered.xml.find(">black</"), std::string::npos) << filtered.xml;

  const RpcResponse created = server.Process("creator", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="create">
      <edit-config><target><candidate/></target><config>
        <preferences xmlns="urn:leaf-list-test"><color>blue</color></preferences>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(created.xml.find("<ok/>"), std::string::npos) << created.xml;

  const std::string before = stores.Read(Datastore::kCandidate).ToXml();
  const RpcResponse wrong_instance = server.Process("creator", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="wrong">
      <edit-config><target><candidate/></target><config>
        <preferences xmlns="urn:leaf-list-test"><color>green</color></preferences>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(wrong_instance.xml.find("access-denied"), std::string::npos)
      << wrong_instance.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml(), before);

  const RpcResponse deleted = server.Process("deleter", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="delete">
      <edit-config><target><candidate/></target><config>
        <preferences xmlns="urn:leaf-list-test">
          <color xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
                 nc:operation="delete">blue</color>
        </preferences>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(deleted.xml.find("<ok/>"), std::string::npos) << deleted.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml().find(">blue</"),
            std::string::npos);
}

TEST(NetconfServerTest, AuthorizesExplicitDefaultsWithoutPhantomWrites) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("defaults.yang", R"yang(module defaults {
    yang-version 1.1; namespace "urn:defaults-test"; prefix d;
    container settings {
      leaf label { type string; }
      leaf mode { type string; default "auto"; }
    }
  })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  ASSERT_TRUE(compilation);
  config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto initial = config::ParseDatastoreXml(schema, R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <settings xmlns="urn:defaults-test"><label>old</label></settings>
    </config>)xml").document;
  ASSERT_TRUE(initial);
  DatastoreManager stores(schema, *initial);
  NacmPolicy policy;
  policy.set_write_default(AccessAction::kDeny);
  for (const std::string& user : {"updater", "creator", "deleter"})
    policy.AddUserToGroup(user, user);
  const std::string settings = "/{urn:defaults-test}settings";
  policy.AddRule({"update-label", "updater", "", settings +
                      "/{urn:defaults-test}label",
                  AccessMask(AccessOperation::kUpdate), AccessAction::kPermit});
  policy.AddRule({"create-mode", "creator", "", settings +
                      "/{urn:defaults-test}mode",
                  AccessMask(AccessOperation::kCreate), AccessAction::kPermit});
  policy.AddRule({"delete-mode", "deleter", "", settings +
                      "/{urn:defaults-test}mode",
                  AccessMask(AccessOperation::kDelete), AccessAction::kPermit});
  NetconfServer server(stores, &policy);

  // The virtual default is part of the effective view, not stored
  // configuration, and therefore must not add a phantom write authorization
  // check to an unrelated explicit edit.
  const RpcResponse updated = server.Process("updater", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="update">
      <edit-config><target><candidate/></target><config>
        <settings xmlns="urn:defaults-test"><label>new</label></settings>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(updated.xml.find("<ok/>"), std::string::npos) << updated.xml;

  const std::string before = stores.Read(Datastore::kCandidate).ToXml();
  const RpcResponse denied = server.Process("updater", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="deny">
      <edit-config><target><candidate/></target><config>
        <settings xmlns="urn:defaults-test"><mode>auto</mode></settings>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(denied.xml.find("access-denied"), std::string::npos) << denied.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml(), before);

  const RpcResponse created = server.Process("creator", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="create">
      <edit-config><target><candidate/></target><config>
        <settings xmlns="urn:defaults-test"><mode>auto</mode></settings>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(created.xml.find("<ok/>"), std::string::npos) << created.xml;
  EXPECT_NE(stores.Read(Datastore::kCandidate).ToXml().find(">auto</"),
            std::string::npos);

  const RpcResponse deleted = server.Process("deleter", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="delete">
      <edit-config><target><candidate/></target><config>
        <settings xmlns="urn:defaults-test">
          <mode xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
                nc:operation="delete">auto</mode>
        </settings>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(deleted.xml.find("<ok/>"), std::string::npos) << deleted.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml().find(">auto</"),
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

TEST(NetconfServerTest, DeniesEverySupportedStandardOperationBeforeDispatch) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  NacmPolicy policy;
  policy.set_exec_default(AccessAction::kDeny);
  NotificationManager notifications(&policy);
  ASSERT_TRUE(notifications.AddStream({}));
  MinimalOperationalProvider operational;
  NetconfServer server(stores, &policy, nullptr, &notifications, std::nullopt,
                       &operational);
  struct Operation {
    std::string_view name;
    std::string_view xml_namespace;
  };
  constexpr std::string_view netconf =
      "urn:ietf:params:xml:ns:netconf:base:1.0";
  constexpr std::string_view notification =
      "urn:ietf:params:xml:ns:netconf:notification:1.0";
  constexpr std::string_view monitoring =
      "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring";
  constexpr std::string_view nmda =
      "urn:ietf:params:xml:ns:yang:ietf-netconf-nmda";
  const std::vector<Operation> operations = {
      {"get-config", netconf}, {"get", netconf},
      {"edit-config", netconf}, {"lock", netconf},
      {"unlock", netconf}, {"validate", netconf},
      {"discard-changes", netconf}, {"commit", netconf},
      {"cancel-commit", netconf}, {"copy-config", netconf},
      {"delete-config", netconf}, {"kill-session", netconf},
      {"create-subscription", notification}, {"get-schema", monitoring},
      {"get-data", nmda}, {"edit-data", nmda}};

  for (const Operation& operation : operations) {
    const std::string request =
        "<rpc xmlns=\"" + std::string(netconf) + "\" message-id=\"deny-" +
        std::string(operation.name) + "\"><op:" +
        std::string(operation.name) + " xmlns:op=\"" +
        std::string(operation.xml_namespace) + "\"/></rpc>";
    const RpcResponse denied = server.Process("guest", request);
    EXPECT_NE(denied.xml.find("<error-type>application</error-type>"),
              std::string::npos) << operation.name << ": " << denied.xml;
    EXPECT_NE(denied.xml.find("<error-tag>access-denied</error-tag>"),
              std::string::npos) << operation.name << ": " << denied.xml;
    const std::string error_path = "/nc:rpc/" +
        std::string(operation.xml_namespace == netconf ? "nc:" : "op:") +
        std::string(operation.name) + "</error-path>";
    EXPECT_NE(denied.xml.find(error_path),
              std::string::npos) << operation.name << ": " << denied.xml;
    if (operation.xml_namespace != netconf) {
      EXPECT_NE(denied.xml.find("xmlns:op=\"" +
                    std::string(operation.xml_namespace) + "\""),
                std::string::npos) << operation.name << ": " << denied.xml;
    }
    EXPECT_EQ(denied.xml.find("<error-info>"), std::string::npos)
        << operation.name << ": " << denied.xml;
  }
  EXPECT_EQ(policy.counters().denied_operations, operations.size());

  const RpcResponse close = server.Process(
      "guest", "<rpc xmlns=\"" + std::string(netconf) +
          "\" message-id=\"close\"><close-session/></rpc>");
  EXPECT_TRUE(close.close_session);
  EXPECT_NE(close.xml.find("<ok/>"), std::string::npos) << close.xml;
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

TEST(NetconfServerTest, AppliesNacmToDatastoreSidesOfUrlCopies) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  MemoryUrlProvider urls;
  urls.values["memory:target"] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:rpc-test"><hostname>remote-old</hostname></system>
    </config>)xml";
  urls.values["memory:source"] = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:rpc-test"><hostname>remote-new</hostname></system>
    </config>)xml";
  NacmPolicy policy;
  policy.set_read_default(AccessAction::kPermit);
  policy.set_write_default(AccessAction::kPermit);
  policy.AddUserToGroup("alice", "users");
  policy.AddRule({"hide-device", "users", "", "/{urn:rpc-test}device",
                  AccessMask(AccessOperation::kRead), AccessAction::kDeny});
  NetconfServer server(stores, &policy, &urls);

  const RpcResponse to_url = server.Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="to-url">
      <copy-config><target><url>memory:target</url></target>
        <source><running/></source></copy-config>
    </rpc>)xml");
  EXPECT_NE(to_url.xml.find("<ok/>"), std::string::npos) << to_url.xml;
  EXPECT_EQ(urls.values["memory:target"].find("device"), std::string::npos)
      << urls.values["memory:target"];

  policy.AddRule({"deny-hostname", "users", "",
                  "/{urn:rpc-test}system/{urn:rpc-test}hostname",
                  static_cast<std::uint8_t>(
                      AccessMask(AccessOperation::kCreate) |
                      AccessMask(AccessOperation::kUpdate) |
                      AccessMask(AccessOperation::kDelete)),
                  AccessAction::kDeny});
  const std::string before = stores.Read(Datastore::kCandidate).ToXml();
  const RpcResponse from_url = server.Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="from-url">
      <copy-config><target><candidate/></target>
        <source><url>memory:source</url></source></copy-config>
    </rpc>)xml");
  EXPECT_NE(from_url.xml.find("access-denied"), std::string::npos)
      << from_url.xml;
  EXPECT_EQ(stores.Read(Datastore::kCandidate).ToXml(), before);
}

TEST(NetconfServerTest, FailsRemoteCopiesBeforeTargetMutation) {
  VectorDiagnosticSink diagnostics;
  auto fixture = BuildServerFixture(&diagnostics);
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->initial);
  MemoryUrlProvider urls;
  const std::string source_xml = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:rpc-test"><hostname>remote-new</hostname></system>
    </config>)xml";
  const std::string target_xml = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:rpc-test"><hostname>remote-old</hostname></system>
    </config>)xml";
  urls.values["memory:source"] = source_xml;
  urls.values["memory:target"] = target_xml;
  NacmPolicy policy;
  policy.set_write_default(AccessAction::kPermit);
  NetconfServer server(stores, &policy, &urls);
  const auto copy = [&](std::string_view message_id) {
    return server.Process("alice",
        "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
        "message-id=\"" + std::string(message_id) + "\">"
        "<copy-config><target><url>memory:target</url></target>"
        "<source><url>memory:source</url></source></copy-config></rpc>");
  };

  urls.read_failures["memory:source"] =
      UrlResult{{}, "source temporarily unavailable", "resource-denied"};
  const RpcResponse source_failed = copy("source-failed");
  EXPECT_NE(source_failed.xml.find("<error-tag>resource-denied</error-tag>"),
            std::string::npos) << source_failed.xml;
  EXPECT_NE(source_failed.xml.find("source temporarily unavailable"),
            std::string::npos) << source_failed.xml;
  EXPECT_EQ(urls.write_calls["memory:target"], 0u);
  EXPECT_EQ(urls.values["memory:target"], target_xml);
  EXPECT_EQ(policy.counters().denied_data_writes, 0u);
  urls.read_failures.clear();

  urls.read_failures["memory:target"] =
      UrlResult{{}, "target cannot be inspected", "lock-denied"};
  const RpcResponse target_read_failed = copy("target-read-failed");
  EXPECT_NE(target_read_failed.xml.find("<error-tag>lock-denied</error-tag>"),
            std::string::npos) << target_read_failed.xml;
  EXPECT_NE(target_read_failed.xml.find("target cannot be inspected"),
            std::string::npos) << target_read_failed.xml;
  EXPECT_EQ(urls.write_calls["memory:target"], 0u);
  EXPECT_EQ(urls.values["memory:target"], target_xml);
  EXPECT_EQ(policy.counters().denied_data_writes, 0u);
  urls.read_failures.clear();

  urls.write_failures["memory:target"] =
      UrlResult{{}, "target write rejected", "resource-denied"};
  const RpcResponse write_failed = copy("write-failed");
  EXPECT_NE(write_failed.xml.find("<error-tag>resource-denied</error-tag>"),
            std::string::npos) << write_failed.xml;
  EXPECT_NE(write_failed.xml.find("target write rejected"), std::string::npos)
      << write_failed.xml;
  EXPECT_EQ(urls.write_calls["memory:target"], 1u);
  EXPECT_EQ(urls.values["memory:target"], target_xml);
  EXPECT_EQ(policy.counters().denied_data_writes, 0u);
}

}  // namespace
}  // namespace yang::netconf
