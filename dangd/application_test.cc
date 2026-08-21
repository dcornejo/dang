// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/application.h"
#include "dangd/test_plugins/plugin_test_support.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <dlfcn.h>

#include <gtest/gtest.h>

namespace dangd {
namespace {

class TemporaryInputs {
 public:
  TemporaryInputs() {
    directory_ = std::filesystem::temp_directory_path() /
                 ("dangd-test-" + std::to_string(++sequence_));
    std::filesystem::create_directories(directory_);
  }
  ~TemporaryInputs() {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }
  std::filesystem::path Write(std::string name, std::string_view contents) {
    const auto path = directory_ / std::move(name);
    std::ofstream output(path, std::ios::binary);
    output << contents;
    return path;
  }
  std::filesystem::path Path(std::string name) const {
    return directory_ / std::move(name);
  }

 private:
  inline static unsigned int sequence_ = 0;
  std::filesystem::path directory_;
};

constexpr std::string_view kModel = R"yang(
module appliance {
  yang-version 1.1;
  namespace "urn:example:appliance";
  prefix a;
  container system {
    leaf hostname { type string; mandatory true; }
  }
}
)yang";

constexpr std::string_view kConfig = R"xml(
<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <system xmlns="urn:example:appliance"><hostname>edge-1</hostname></system>
</config>
)xml";

ApplicationOptions Options(TemporaryInputs& inputs) {
  return {.model = inputs.Write("appliance.yang", kModel),
          .configuration = inputs.Write("config.xml", kConfig),
          .recovery_users = {"alice", "one"}};
}

yang::netconf::RpcResponse SetProviderMode(Application& application,
                                           std::string_view mode) {
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  return application.server().Process(
      session,
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"edit\"><edit-config><target><candidate/></target>"
      "<config><provider-settings xmlns=\"urn:dangd:test:provider\">"
      "<mode>" + std::string(mode) + "</mode></provider-settings></config>"
      "</edit-config></rpc>");
}

yang::netconf::RpcResponse Commit(Application& application) {
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  return application.server().Process(
      session,
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"commit\"><commit/></rpc>");
}

TEST(DangdApplicationTest, LoadsModelAndCompleteConfiguration) {
  TemporaryInputs inputs;
  auto loaded = Application::Load(Options(inputs));
  ASSERT_NE(loaded.application, nullptr);
  EXPECT_TRUE(loaded.errors.empty());
  EXPECT_TRUE(loaded.application->schema()
                  .FindRoot({"urn:example:appliance", "system"})
                  .has_value());
  EXPECT_TRUE(loaded.application->schema()
                  .FindRoot({"urn:ietf:params:xml:ns:yang:ietf-yang-library",
                             "yang-library"})
                  .has_value());
  const auto library = loaded.application->server().Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="nmda">
      <get/>
    </rpc>)xml");
  EXPECT_NE(library.xml.find("<name>ds:intended</name>"), std::string::npos)
      << library.xml;
  EXPECT_NE(library.xml.find("<name>ietf-netconf-nmda</name>"),
            std::string::npos) << library.xml;
  EXPECT_NE(library.xml.find("<name>ietf-origin</name>"), std::string::npos)
      << library.xml;
  EXPECT_NE(library.xml.find("<feature>xpath</feature>"), std::string::npos)
      << library.xml;
}

TEST(DangdApplicationTest, RetrievesAndEditsConventionalNmdaDatastores) {
  TemporaryInputs inputs;
  auto loaded = Application::Load(Options(inputs));
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};

  const auto intended = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="get-data"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:intended</datastore>
        <subtree-filter><system xmlns="urn:example:appliance"/></subtree-filter>
        <config-filter>true</config-filter>
        <max-depth>2</max-depth>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(intended.xml.find(">edge-1</"), std::string::npos) << intended.xml;
  EXPECT_NE(intended.xml.find("ietf-netconf-nmda"), std::string::npos)
      << intended.xml;

  const auto operational = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="operational"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(operational.xml.find("edge-1"), std::string::npos)
      << operational.xml;
  EXPECT_NE(operational.xml.find("yang-library"), std::string::npos)
      << operational.xml;
  EXPECT_NE(operational.xml.find("denied-operations"), std::string::npos)
      << operational.xml;

  const auto origins = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="origins"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <config-filter>true</config-filter><with-origin/>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(origins.xml.find("or:origin=\"or:intended\""), std::string::npos)
      << origins.xml;

  const auto other_origins = loaded.application->server().Process(
      session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="origin-filter"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:or="urn:ietf:params:xml:ns:yang:ietf-origin">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <origin-filter>or:unknown</origin-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(other_origins.xml.find("edge-1"), std::string::npos)
      << other_origins.xml;
  EXPECT_NE(other_origins.xml.find("yang-library"), std::string::npos)
      << other_origins.xml;

  const auto edit = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="edit-data"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <edit-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:candidate</datastore>
        <config><system xmlns="urn:example:appliance">
          <hostname>edge-nmda</hostname>
        </system></config>
      </edit-data>
    </rpc>)xml");
  ASSERT_NE(edit.xml.find("<ok/>"), std::string::npos) << edit.xml;
  EXPECT_NE(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kCandidate)
                .ToXml()
                .find("edge-nmda"),
            std::string::npos);

  const auto rejected = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="readonly"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <edit-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:intended</datastore><config/>
      </edit-data>
    </rpc>)xml");
  EXPECT_NE(rejected.xml.find("<error-tag>invalid-value</error-tag>"),
            std::string::npos) << rejected.xml;
}

TEST(DangdApplicationTest, UsesSecureNacmDefaultsWhenSubtreeIsAbsent) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.recovery_users.clear();
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);
  const auto denied = loaded.application->server().Process("guest", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="secure">
      <edit-config><target><candidate/></target><config>
        <system xmlns="urn:example:appliance"><hostname>changed</hostname></system>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(denied.xml.find("access-denied"), std::string::npos) << denied.xml;
}

TEST(DangdApplicationTest, LoadsNacmAndUsesAuthenticatedSessionIdentity) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.nacm_configuration = inputs.Write("nacm.xml", R"xml(
    <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
      <read-default>permit</read-default><write-default>deny</write-default>
      <exec-default>deny</exec-default>
      <groups><group><name>admins</name><user-name>alice</user-name>
      </group></groups>
      <rule-list><name>admin</name><group>admins</group>
        <rule><name>netconf</name><module-name>ietf-netconf</module-name>
          <rpc-name>*</rpc-name><access-operations>exec</access-operations>
          <action>permit</action></rule>
        <rule><name>data</name><module-name>appliance</module-name>
          <access-operations>*</access-operations><action>permit</action></rule>
      </rule-list>
    </nacm>)xml");
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);

  const std::string commit =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"nacm\"><commit/></rpc>";
  yang::netconf::RpcSessionContext alice{1, "alice", "alice", {}};
  yang::netconf::RpcSessionContext bob{2, "bob", "bob", {}};
  EXPECT_NE(loaded.application->server().Process(alice, commit).xml.find(
                "<ok/>"),
            std::string::npos);
  EXPECT_NE(loaded.application->server().Process(bob, commit).xml.find(
                "access-denied"),
            std::string::npos);
}

TEST(DangdApplicationTest, EmitsSafeRecoveryAuditRecords) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.recovery_users = {"line\nuser"};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr)
      << testing::PrintToString(loaded.errors);
  const std::string request =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"audit\"><get/></rpc>";
  (void)loaded.application->server().Process(
      {71, "ordinary", "ordinary", {}}, request);
  (void)loaded.application->server().Process(
      {72, "line\nuser", "line\nuser", {}}, request);

  const std::vector<std::string> records =
      loaded.application->DrainRecoveryAuditRecords();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_NE(records.front().find("session=72"), std::string::npos);
  EXPECT_NE(records.front().find("user=line%0Auser"), std::string::npos);
  EXPECT_EQ(records.front().find('\n'), std::string::npos);
  EXPECT_TRUE(loaded.application->DrainRecoveryAuditRecords().empty());
}

TEST(DangdApplicationTest, SeedsAndCommitsDatastoreManagedNacm) {
  TemporaryInputs inputs;
  constexpr std::string_view model = R"yang(module managed-appliance {
    yang-version 1.1; namespace "urn:managed-appliance"; prefix ma;
    import ietf-netconf-acm { prefix nacm; revision-date "2018-02-14"; }
    container system { leaf hostname { type string; mandatory true; } }
  })yang";
  const std::filesystem::path source = DANG_TEST_SOURCE_DIR;
  ApplicationOptions options{
      .model = inputs.Write("managed-appliance.yang", model),
      .search_paths = {source / "dangd/models"},
      .configuration = inputs.Write(
          "config.xml",
          "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
          "<system xmlns=\"urn:managed-appliance\"><hostname>edge</hostname>"
          "</system></config>"),
      .nacm_configuration = source / "dangd/examples/nacm.xml"};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  EXPECT_NE(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kRunning)
                .ToXml()
                .find("<nacm"),
            std::string::npos);

  yang::netconf::RpcSessionContext alice{1, "alice", "alice", {}};
  yang::netconf::RpcSessionContext bob{2, "bob", "bob", {}};
  const std::string commit =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"c\"><commit/></rpc>";
  EXPECT_NE(loaded.application->server().Process(bob, commit).xml.find(
                "access-denied"),
            std::string::npos);
  ASSERT_NE(loaded.application->server().Process(alice, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="e">
      <edit-config><target><candidate/></target><config>
        <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
          <exec-default>permit</exec-default>
        </nacm>
      </config></edit-config>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);
  ASSERT_NE(loaded.application->server().Process(alice, commit).xml.find(
                "<ok/>"),
            std::string::npos);
  EXPECT_NE(loaded.application->server().Process(bob, commit).xml.find(
                "<ok/>"),
            std::string::npos);
}

TEST(DangdApplicationTest, LoadsPluginModelAndRejectsPluginInvalidCommit) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  EXPECT_TRUE(loaded.application->schema()
                  .FindRoot({"urn:dangd:example-plugin", "plugin-settings"})
                  .has_value());

  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto edit = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
      <edit-config><target><candidate/></target><config>
        <plugin-settings xmlns="urn:dangd:example-plugin">
          <mode>reject</mode>
        </plugin-settings>
      </config></edit-config>
    </rpc>)xml");
  ASSERT_NE(edit.xml.find("<ok/>"), std::string::npos) << edit.xml;
  const auto validate = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="v">
      <validate><source><candidate/></source></validate>
    </rpc>)xml");
  EXPECT_NE(validate.xml.find("dangd-example-plugin"), std::string::npos)
      << validate.xml;
  const auto commit = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="2">
      <commit/>
    </rpc>)xml");
  EXPECT_NE(commit.xml.find("operation-failed"), std::string::npos) << commit.xml;
  EXPECT_NE(commit.xml.find("dangd-example-plugin"), std::string::npos)
      << commit.xml;
  EXPECT_EQ(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kRunning)
                .ToXml()
                .find("plugin-settings"),
            std::string::npos);
}

TEST(DangdApplicationTest, IpManagementPluginPublishesRfc8344AndPrintsApplyPlan) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_IP_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  EXPECT_TRUE(loaded.application->schema()
                  .FindRoot({"urn:ietf:params:xml:ns:yang:ietf-interfaces",
                             "interfaces"})
                  .has_value());

  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto library = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="library">
      <get/>
    </rpc>)xml");
  EXPECT_NE(library.xml.find("<name>ietf-interfaces</name>"), std::string::npos)
      << library.xml;
  EXPECT_NE(library.xml.find("<name>ietf-ip</name>"), std::string::npos)
      << library.xml;

  const auto edit = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="ip-edit">
      <edit-config><target><candidate/></target><config>
        <interfaces xmlns="urn:ietf:params:xml:ns:yang:ietf-interfaces">
          <interface>
            <name>eth0</name>
            <type xmlns:if="urn:ietf:params:xml:ns:yang:ietf-interfaces">if:interface-type</type>
            <enabled>true</enabled>
            <ipv4 xmlns="urn:ietf:params:xml:ns:yang:ietf-ip">
              <enabled>true</enabled>
              <address><ip>192.0.2.1</ip><prefix-length>24</prefix-length></address>
            </ipv4>
          </interface>
        </interfaces>
      </config></edit-config>
    </rpc>)xml");
  ASSERT_NE(edit.xml.find("<ok/>"), std::string::npos) << edit.xml;

  testing::internal::CaptureStderr();
  const auto commit = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="ip-commit">
      <commit/>
    </rpc>)xml");
  const std::string actions = testing::internal::GetCapturedStderr();
  ASSERT_NE(commit.xml.find("<ok/>"), std::string::npos) << commit.xml;
  EXPECT_NE(actions.find("ip-management: create"), std::string::npos)
      << actions;
  EXPECT_NE(actions.find("eth0"), std::string::npos) << actions;
  EXPECT_NE(actions.find("192.0.2.1"), std::string::npos) << actions;
  EXPECT_NE(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kRunning)
                .ToXml()
                .find("192.0.2.1"),
            std::string::npos);
  const auto state = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="ip-state"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <config-filter>false</config-filter>
        <subtree-filter>
          <interfaces-state
              xmlns="urn:ietf:params:xml:ns:yang:ietf-interfaces"/>
        </subtree-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(state.xml.find("interfaces-state"), std::string::npos) << state.xml;
  EXPECT_NE(state.xml.find("eth0"), std::string::npos) << state.xml;
  EXPECT_NE(state.xml.find("oper-status>up"), std::string::npos) << state.xml;
}

TEST(DangdApplicationTest, AppliesDependentPluginsInDependencyOrder) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_CONSUMER_PLUGIN_PATH,
                     DANG_TEST_PROVIDER_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  test_plugin::ResetTrace();

  ASSERT_NE(SetProviderMode(*loaded.application, "active").xml.find("<ok/>"),
            std::string::npos);
  const auto commit = Commit(*loaded.application);
  ASSERT_NE(commit.xml.find("<ok/>"), std::string::npos) << commit.xml;
  EXPECT_EQ(test_plugin::Trace(),
            (std::vector<std::string>{
                "provider.prepare", "consumer.prepare", "provider.validate",
                "consumer.validate", "provider.apply", "consumer.apply",
                "consumer.release", "provider.release"}));
}

TEST(DangdApplicationTest, DispatchesPluginOwnedSchemaRpc) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_PROVIDER_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="status">
      <provider-status xmlns="urn:dangd:test:provider"/>
    </rpc>)xml");
  EXPECT_NE(response.xml.find("<status"), std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("ready"), std::string::npos) << response.xml;
}

TEST(DangdApplicationTest, ValidatesEveryPluginBeforeApplyingAnyPlugin) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_CONSUMER_PLUGIN_PATH,
                     DANG_TEST_PROVIDER_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);
  test_plugin::ResetTrace();

  ASSERT_NE(SetProviderMode(*loaded.application, "consumer-validate-fail")
                .xml.find("<ok/>"),
            std::string::npos);
  const auto commit = Commit(*loaded.application);
  EXPECT_NE(commit.xml.find("provider mode is incompatible"), std::string::npos)
      << commit.xml;
  EXPECT_NE(commit.xml.find("/provider:provider-settings/provider:mode"),
            std::string::npos) << commit.xml;
  EXPECT_EQ(test_plugin::Trace(),
            (std::vector<std::string>{
                "provider.prepare", "consumer.prepare", "provider.validate",
                "consumer.validate", "consumer.release", "provider.release"}));
  EXPECT_TRUE(test_plugin::Active("provider").empty());
  EXPECT_TRUE(test_plugin::Active("consumer").empty());
}

TEST(DangdApplicationTest, RollsBackAppliedDependencyAfterConsumerFailure) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_CONSUMER_PLUGIN_PATH,
                     DANG_TEST_PROVIDER_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);
  test_plugin::ResetTrace();

  ASSERT_NE(SetProviderMode(*loaded.application, "consumer-apply-fail")
                .xml.find("<ok/>"),
            std::string::npos);
  const auto commit = Commit(*loaded.application);
  EXPECT_NE(commit.xml.find("simulated consumer hardware failure"),
            std::string::npos) << commit.xml;
  EXPECT_EQ(test_plugin::Trace(),
            (std::vector<std::string>{
                "provider.prepare", "consumer.prepare", "provider.validate",
                "consumer.validate", "provider.apply", "consumer.apply",
                "provider.rollback", "consumer.release", "provider.release"}));
  EXPECT_EQ(test_plugin::Active("provider").find("consumer-apply-fail"),
            std::string::npos);
  EXPECT_NE(test_plugin::Active("provider").find("edge-1"),
            std::string::npos);
  EXPECT_TRUE(test_plugin::Active("consumer").empty());
  EXPECT_EQ(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kRunning)
                .ToXml()
                .find("consumer-apply-fail"),
            std::string::npos);
}

TEST(DangdApplicationTest, ReportsApplyAndRollbackFailuresTogether) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_CONSUMER_PLUGIN_PATH,
                     DANG_TEST_PROVIDER_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);
  test_plugin::ResetTrace();

  ASSERT_NE(SetProviderMode(*loaded.application,
                            "consumer-apply-rollback-fail")
                .xml.find("<ok/>"),
            std::string::npos);
  const auto commit = Commit(*loaded.application);
  EXPECT_NE(commit.xml.find("simulated consumer hardware failure"),
            std::string::npos) << commit.xml;
  EXPECT_NE(commit.xml.find("simulated provider rollback failure"),
            std::string::npos) << commit.xml;
  EXPECT_NE(commit.xml.find("plugin-rollback-failed"), std::string::npos)
      << commit.xml;
  EXPECT_NE(test_plugin::Active("provider").find(
                "consumer-apply-rollback-fail"),
            std::string::npos);
  EXPECT_EQ(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kRunning)
                .ToXml()
                .find("consumer-apply-rollback-fail"),
            std::string::npos);
}

TEST(DangdApplicationTest, RollsPluginBackWithCancelledConfirmedCommit) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  ASSERT_NE(loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
      <edit-config><target><candidate/></target><config>
        <plugin-settings xmlns="urn:dangd:example-plugin"><mode>active</mode>
        </plugin-settings>
      </config></edit-config>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);
  ASSERT_NE(loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="2">
      <commit><confirmed/><confirm-timeout>60</confirm-timeout></commit>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);

  void* handle = dlopen(DANG_TEST_PLUGIN_PATH, RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(handle, nullptr);
  using ActiveConfiguration = const char* (*)();
  auto active = reinterpret_cast<ActiveConfiguration>(
      dlsym(handle, "dang_example_active_configuration"));
  ASSERT_NE(active, nullptr);
  EXPECT_NE(std::string(active()).find("<mode>active</mode>"),
            std::string::npos);

  ASSERT_NE(loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="3">
      <cancel-commit/>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);
  EXPECT_EQ(std::string(active()).find("plugin-settings"), std::string::npos);
  dlclose(handle);
}

TEST(DangdApplicationTest, AdvertisesPluginSourceThroughYangLibraryGet) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto get = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
      <get/>
    </rpc>)xml");
  EXPECT_NE(get.xml.find("ietf-yang-library"), std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find("dangd-example-plugin"), std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find("plugin:dangd-example-plugin"), std::string::npos)
      << get.xml;
  EXPECT_NE(get.xml.find("<content-id>"), std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find("<modules-state"), std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find("<module-set-id>"), std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find("<netconf-state"), std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find("<identifier>ietf-netconf-monitoring</identifier>"),
            std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find("<location>NETCONF</location>"), std::string::npos)
      << get.xml;
  EXPECT_NE(get.xml.find("<conformance-type>implement</conformance-type>"),
            std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find("<conformance-type>import</conformance-type>"),
            std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find("<name>ds:operational</name>"), std::string::npos)
      << get.xml;
  EXPECT_NE(get.xml.find("<denied-operations>0</denied-operations>"),
            std::string::npos) << get.xml;
  EXPECT_NE(loaded.application->server().ServerHello(9).find(
                "capability:yang-library:1.1?revision=2019-01-04&amp;content-id="),
            std::string::npos);
  EXPECT_NE(loaded.application->server().ServerHello(9).find(
                "ietf-netconf-monitoring?module=ietf-netconf-monitoring&amp;"
                "revision=2010-10-04"),
            std::string::npos);

  const auto get_config = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="2">
      <get-config><source><running/></source></get-config>
    </rpc>)xml");
  EXPECT_EQ(get_config.xml.find("<yang-library"), std::string::npos)
      << get_config.xml;
  EXPECT_EQ(get_config.xml.find("<modules-state"), std::string::npos)
      << get_config.xml;
  EXPECT_EQ(get_config.xml.find("<netconf-state"), std::string::npos)
      << get_config.xml;
}

TEST(DangdApplicationTest, RetrievesBuiltInAndPluginYangSources) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto retrieve = [&](std::string_view body) {
    return loaded.application->server().Process(
        session,
        "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
        "message-id=\"schema\"><get-schema "
        "xmlns=\"urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring\">" +
            std::string(body) + "</get-schema></rpc>");
  };
  const auto root = retrieve(
      "<identifier>appliance</identifier><format>yang</format>");
  EXPECT_NE(root.xml.find("module appliance"), std::string::npos) << root.xml;
  const auto plugin = retrieve(
      "<identifier>dangd-example-plugin</identifier>"
      "<version>2026-08-13</version>");
  EXPECT_NE(plugin.xml.find("module dangd-example-plugin"), std::string::npos)
      << plugin.xml;
  const auto monitoring = retrieve(
      "<identifier>ietf-netconf-monitoring</identifier>"
      "<version>2010-10-04</version>");
  EXPECT_NE(monitoring.xml.find("module ietf-netconf-monitoring"),
            std::string::npos) << monitoring.xml;
  EXPECT_NE(retrieve("<identifier>missing</identifier>")
                .xml.find("invalid-value"),
            std::string::npos);
  EXPECT_NE(retrieve("<identifier>appliance</identifier><format>yin</format>")
                .xml.find("invalid-value"),
            std::string::npos);
}

TEST(DangdApplicationTest, PublishesDeviationRelationshipsInBothLibraries) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_DEVIATION_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto get = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
      <get/>
    </rpc>)xml");
  EXPECT_NE(get.xml.find(
                "<name>appliance</name><namespace>urn:example:appliance"
                "</namespace><deviation>dangd-test-deviation</deviation>"),
            std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find(
                "<deviation><name>dangd-test-deviation</name>"
                "<revision>2026-08-20</revision></deviation>"),
            std::string::npos) << get.xml;
  EXPECT_NE(get.xml.find(
                "<name>dangd-test-deviation</name><revision>2026-08-20"
                "</revision><namespace>urn:dangd:test:deviation</namespace>"
                "<conformance-type>implement</conformance-type>"),
            std::string::npos) << get.xml;
}

TEST(DangdApplicationTest, PublishesYangLibraryUpdateToSubscribers) {
  TemporaryInputs inputs;
  auto loaded = Application::Load(Options(inputs));
  ASSERT_NE(loaded.application, nullptr);
  yang::netconf::RpcSessionContext session{42, "alice", "alice", {}};
  const auto subscribe = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="sub">
      <create-subscription
        xmlns="urn:ietf:params:xml:ns:netconf:notification:1.0"/>
    </rpc>)xml");
  ASSERT_NE(subscribe.xml.find("<ok/>"), std::string::npos) << subscribe.xml;
  ASSERT_TRUE(loaded.application->PublishYangLibraryUpdate("replacement-id"));
  const auto notifications =
      loaded.application->server().DrainNotifications(session.session_id);
  ASSERT_EQ(notifications.size(), 2u);
  EXPECT_NE(notifications.front().find("yang-library-update"),
            std::string::npos);
  EXPECT_NE(notifications.front().find("<content-id>replacement-id</content-id>"),
            std::string::npos);
  EXPECT_NE(notifications.back().find("yang-library-change"),
            std::string::npos);
  EXPECT_NE(notifications.back().find(
                "<module-set-id>replacement-id</module-set-id>"),
            std::string::npos);
  EXPECT_TRUE(loaded.application->PublishYangLibraryUpdate(
      loaded.application->yang_library_content_id()));
  EXPECT_TRUE(loaded.application->server()
                  .DrainNotifications(session.session_id)
                  .empty());
}

TEST(DangdApplicationTest, AtomicallyReloadsSchemaWithRunningConfiguration) {
  TemporaryInputs inputs;
  const auto model = inputs.Write("reload.yang", R"yang(module reloadable {
    yang-version 1.1; namespace "urn:dangd:reload"; prefix r;
    revision 2026-08-19;
    container system { leaf hostname { type string; mandatory true; } }
  })yang");
  const auto config = inputs.Write(
      "reload.xml", "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<system xmlns=\"urn:dangd:reload\"><hostname>edge</hostname></system>"
      "</config>");
  ApplicationOptions options{.model = model, .configuration = config};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  const std::string old_id = loaded.application->yang_library_content_id();

  inputs.Write("reload.yang", R"yang(module reloadable {
    yang-version 1.1; namespace "urn:dangd:reload"; prefix r;
    revision 2026-08-20;
    container system {
      leaf hostname { type string; mandatory true; }
      leaf description { type string; }
    }
  })yang");
  auto replacement = Application::Reload(options, *loaded.application);
  ASSERT_NE(replacement.application, nullptr)
      << testing::PrintToString(replacement.errors);
  EXPECT_NE(replacement.application->yang_library_content_id(), old_id);
  EXPECT_NE(replacement.application->datastores()
                .Read(yang::netconf::Datastore::kRunning)
                .ToXml()
                .find("<hostname>edge</hostname>"),
            std::string::npos);
  const std::string replacement_id =
      replacement.application->yang_library_content_id();

  inputs.Write("reload.yang", R"yang(module reloadable {
    yang-version 1.1; namespace "urn:dangd:reload"; prefix r;
    revision 2026-08-21;
    container system {
      leaf hostname { type string; mandatory true; }
      leaf required-hardware-id { type string; mandatory true; }
    }
  })yang");
  auto rejected = Application::Reload(options, *replacement.application);
  EXPECT_EQ(rejected.application, nullptr);
  EXPECT_FALSE(rejected.errors.empty());
  EXPECT_EQ(replacement.application->yang_library_content_id(), replacement_id);
}

TEST(DangdApplicationTest, RejectsSchemaInvalidConfiguration) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.configuration = inputs.Write(
      "invalid.xml",
      "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<system xmlns=\"urn:example:appliance\"/></config>");
  auto loaded = Application::Load(options);
  EXPECT_EQ(loaded.application, nullptr);
  ASSERT_FALSE(loaded.errors.empty());
  EXPECT_NE(loaded.errors.front().find("mandatory"), std::string::npos);
}

TEST(DangdApplicationTest, SavesAndRestoresConfiguredStateFile) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.state_file = inputs.Path("state.json");
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);
  EXPECT_FALSE(loaded.application->SaveState().has_value());
  ASSERT_TRUE(std::filesystem::exists(*options.state_file));

  auto restored = Application::Load(options);
  ASSERT_NE(restored.application, nullptr);
  EXPECT_TRUE(restored.errors.empty());
}

TEST(DangdApplicationTest, LiveCommitRestoresSnapshotAndBackendOnSaveFailure) {
  const yang::netconf::SnapshotSaveStage stages[] = {
      yang::netconf::SnapshotSaveStage::kTemporaryWritten,
      yang::netconf::SnapshotSaveStage::kTemporarySynchronized,
      yang::netconf::SnapshotSaveStage::kSnapshotReplaced,
      yang::netconf::SnapshotSaveStage::kDirectorySynchronized};
  unsigned sequence = 0;
  for (const auto interrupted_stage : stages) {
    TemporaryInputs inputs;
    auto options = Options(inputs);
    options.state_file = inputs.Path("state-" + std::to_string(++sequence) +
                                     ".json");
    bool armed = false;
    options.snapshot_save_checkpoint =
        [&](yang::netconf::SnapshotSaveStage stage) {
          return !armed || stage != interrupted_stage;
        };
    auto loaded = Application::Load(options);
    ASSERT_NE(loaded.application, nullptr)
        << testing::PrintToString(loaded.errors);
    yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
    const auto edited = loaded.application->server().Process(session, R"xml(
      <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="edit">
        <edit-config><target><candidate/></target><config>
          <system xmlns="urn:example:appliance"><hostname>edge-2</hostname></system>
        </config></edit-config>
      </rpc>)xml");
    ASSERT_NE(edited.xml.find("<ok/>"), std::string::npos) << edited.xml;
    std::ifstream before_input(*options.state_file, std::ios::binary);
    const std::string durable_before((std::istreambuf_iterator<char>(before_input)),
                                     std::istreambuf_iterator<char>());

    armed = true;
    const auto committed = Commit(*loaded.application);
    ASSERT_NE(committed.xml.find("<rpc-error>"), std::string::npos)
        << committed.xml;
    EXPECT_NE(committed.xml.find("operation-failed"), std::string::npos)
        << committed.xml;
    EXPECT_NE(committed.xml.find("datastore persistence failed"),
              std::string::npos) << committed.xml;
    EXPECT_NE(loaded.application->datastores()
                  .Read(yang::netconf::Datastore::kRunning)
                  .ToXml()
                  .find("edge-1"),
              std::string::npos);
    EXPECT_NE(loaded.application->working_configuration().ToXml().find("edge-1"),
              std::string::npos);
    EXPECT_NE(loaded.application->datastores()
                  .Read(yang::netconf::Datastore::kCandidate)
                  .ToXml()
                  .find("edge-2"),
              std::string::npos);
    std::ifstream after_input(*options.state_file, std::ios::binary);
    const std::string durable_after((std::istreambuf_iterator<char>(after_input)),
                                    std::istreambuf_iterator<char>());
    EXPECT_EQ(durable_after, durable_before);
  }
}

TEST(DangdApplicationTest, PersistsManagedNacmBeforeFirstBootCompletes) {
  TemporaryInputs inputs;
  constexpr std::string_view model = R"yang(module persistent-nacm-appliance {
    yang-version 1.1; namespace "urn:persistent-nacm-appliance"; prefix pna;
    import ietf-netconf-acm { prefix nacm; revision-date "2018-02-14"; }
    container system { leaf hostname { type string; mandatory true; } }
  })yang";
  const std::filesystem::path source = DANG_TEST_SOURCE_DIR;
  ApplicationOptions options{
      .model = inputs.Write("persistent-nacm-appliance.yang", model),
      .search_paths = {source / "dangd/models"},
      .configuration = inputs.Write(
          "persistent-config.xml",
          "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
          "<system xmlns=\"urn:persistent-nacm-appliance\">"
          "<hostname>edge</hostname></system></config>"),
      .nacm_configuration = source / "dangd/examples/nacm.xml"};
  const yang::netconf::SnapshotSaveStage stages[] = {
      yang::netconf::SnapshotSaveStage::kTemporaryWritten,
      yang::netconf::SnapshotSaveStage::kTemporarySynchronized,
      yang::netconf::SnapshotSaveStage::kSnapshotReplaced,
      yang::netconf::SnapshotSaveStage::kDirectorySynchronized};

  unsigned sequence = 0;
  for (const yang::netconf::SnapshotSaveStage interrupted_stage : stages) {
    options.state_file = inputs.Path(
        "first-boot-" + std::to_string(sequence++) + ".json");
    options.snapshot_save_checkpoint =
        [interrupted_stage](yang::netconf::SnapshotSaveStage stage) {
          return stage != interrupted_stage;
        };
    auto interrupted = Application::Load(options);
    EXPECT_EQ(interrupted.application, nullptr);
    ASSERT_FALSE(interrupted.errors.empty());
    EXPECT_NE(interrupted.errors.front().find(
                  "cannot persist initial state file"),
              std::string::npos) << testing::PrintToString(interrupted.errors);

    options.snapshot_save_checkpoint = {};
    auto restarted = Application::Load(options);
    ASSERT_NE(restarted.application, nullptr)
        << testing::PrintToString(restarted.errors);
    EXPECT_TRUE(restarted.errors.empty());
    EXPECT_NE(restarted.application->datastores()
                  .Read(yang::netconf::Datastore::kRunning)
                  .ToXml()
                  .find("<nacm"),
              std::string::npos);
    const auto denied = restarted.application->server().Process("bob", R"xml(
      <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="c">
        <commit/>
      </rpc>)xml");
    EXPECT_NE(denied.xml.find("access-denied"), std::string::npos)
        << denied.xml;
  }
}

TEST(DangdApplicationTest, RejectsCorruptConfiguredStateFile) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.state_file = inputs.Write("state.json", "not-json");
  auto loaded = Application::Load(options);
  EXPECT_EQ(loaded.application, nullptr);
  ASSERT_EQ(loaded.errors.size(), 1u);
  EXPECT_NE(loaded.errors.front().find("cannot restore state file"),
            std::string::npos);
}

TEST(DangdApplicationTest, RunsFramedBase10SessionOverStreams) {
  TemporaryInputs inputs;
  auto loaded = Application::Load(Options(inputs));
  ASSERT_NE(loaded.application, nullptr);
  const std::string client_hello =
      "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<capabilities><capability>urn:ietf:params:netconf:base:1.0"
      "</capability></capabilities></hello>]]>]]>";
  const std::string close_rpc =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"7\"><close-session/></rpc>]]>]]>";
  std::istringstream input(client_hello + close_rpc);
  std::ostringstream output;
  std::ostringstream errors;
  EXPECT_EQ(RunStreamSession(*loaded.application, input, output, errors, 41,
                             "operator"), 0);
  EXPECT_TRUE(errors.str().empty());
  EXPECT_NE(output.str().find("<session-id>41</session-id>"),
            std::string::npos);
  EXPECT_NE(output.str().find("message-id=\"7\"><ok/>"),
            std::string::npos);
}

TEST(DangdApplicationTest, CommitReplacesBackendAndDescribesDeltaInEnglish) {
  TemporaryInputs inputs;
  auto loaded = Application::Load(Options(inputs));
  ASSERT_NE(loaded.application, nullptr);
  const auto edit = loaded.application->server().Process("one", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="8">
      <edit-config><target><candidate/></target><config>
        <system xmlns="urn:example:appliance"><hostname>edge-2</hostname></system>
      </config></edit-config>
    </rpc>)xml");
  ASSERT_NE(edit.xml.find("<ok/>"), std::string::npos);
  EXPECT_TRUE(loaded.application->DrainBackendDeltas().empty());

  const auto commit = loaded.application->server().Process("one", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="9">
      <commit/>
    </rpc>)xml");
  ASSERT_NE(commit.xml.find("<ok/>"), std::string::npos);
  const auto deltas = loaded.application->DrainBackendDeltas();
  ASSERT_EQ(deltas.size(), 1u);
  EXPECT_NE(deltas.front().find("Changed "), std::string::npos);
  EXPECT_NE(deltas.front().find("hostname"), std::string::npos);
  EXPECT_NE(deltas.front().find("\"edge-1\" to \"edge-2\""),
            std::string::npos);
  EXPECT_NE(loaded.application->working_configuration().ToXml().find(
                ">edge-2</"),
            std::string::npos);
}

TEST(DangdApplicationTest, DescribesOrderedMoveDeltaInEnglish) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.model = inputs.Write("ordered.yang", R"yang(module ordered {
    yang-version 1.1; namespace "urn:ordered-test"; prefix o;
    list item { key name; ordered-by user; leaf name { type string; } }
  })yang");
  options.configuration = inputs.Write("ordered.xml", R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <item xmlns="urn:ordered-test"><name>a</name></item>
      <item xmlns="urn:ordered-test"><name>b</name></item>
    </config>)xml");
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);
  const auto edit = loaded.application->server().Process("one", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="move">
      <edit-config><target><candidate/></target><config>
        <item xmlns="urn:ordered-test" xmlns:o="urn:ordered-test"
              xmlns:y="urn:ietf:params:xml:ns:yang:1"
              y:insert="after" y:key="[o:name='b']"><name>a</name></item>
      </config></edit-config>
    </rpc>)xml");
  ASSERT_NE(edit.xml.find("<ok/>"), std::string::npos) << edit.xml;
  const auto commit = loaded.application->server().Process("one", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="commit">
      <commit/>
    </rpc>)xml");
  ASSERT_NE(commit.xml.find("<ok/>"), std::string::npos) << commit.xml;
  const auto deltas = loaded.application->DrainBackendDeltas();
  ASSERT_EQ(deltas.size(), 1u);
  EXPECT_NE(deltas.front().find("Moved "), std::string::npos);
  EXPECT_NE(deltas.front().find("name='b'"), std::string::npos)
      << deltas.front();
  EXPECT_NE(deltas.front().find("position 2 to position 1"), std::string::npos)
      << deltas.front();
}

TEST(DangdApplicationTest, FailedCommitPreservesRunningAndBackendConfiguration) {
  TemporaryInputs inputs;
  auto loaded = Application::Load(Options(inputs));
  ASSERT_NE(loaded.application, nullptr);
  const auto edit = loaded.application->server().Process("one", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="10">
      <edit-config><target><candidate/></target><test-option>set</test-option>
        <config><system xmlns="urn:example:appliance">
          <hostname xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
                    nc:operation="delete">edge-1</hostname>
        </system></config>
      </edit-config>
    </rpc>)xml");
  ASSERT_NE(edit.xml.find("<ok/>"), std::string::npos);
  EXPECT_EQ(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kCandidate)
                .ToXml()
                .find("hostname"),
            std::string::npos);

  const auto commit = loaded.application->server().Process("one", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="11">
      <commit/>
    </rpc>)xml");
  EXPECT_NE(commit.xml.find("<rpc-error>"), std::string::npos);
  EXPECT_NE(commit.xml.find("<error-tag>missing-element</error-tag>"),
            std::string::npos);
  EXPECT_NE(commit.xml.find("mandatory data node is absent"),
            std::string::npos);
  EXPECT_NE(commit.xml.find(
                "mandatory data node is absent (module: appliance, path: "
                "/{urn:example:appliance}system/"
                "{urn:example:appliance}hostname)"),
            std::string::npos);
  EXPECT_NE(commit.xml.find(
                "<error-path xmlns:n0=\"urn:example:appliance\">"
                "/n0:system/n0:hostname</error-path>"),
            std::string::npos);
  EXPECT_NE(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kRunning)
                .ToXml()
                .find(">edge-1</"),
            std::string::npos);
  EXPECT_NE(loaded.application->working_configuration().ToXml().find(
                ">edge-1</"),
            std::string::npos);
  EXPECT_TRUE(loaded.application->DrainBackendDeltas().empty());
}

TEST(DangdApplicationTest, InvalidEditTargetReturnsNetconfError) {
  TemporaryInputs inputs;
  auto loaded = Application::Load(Options(inputs));
  ASSERT_NE(loaded.application, nullptr);
  const auto response = loaded.application->server().Process("one", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="12">
      <edit-config><target><unknown/></target><config>
        <system xmlns="urn:example:appliance"><hostname>edge-2</hostname></system>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(response.xml.find("<rpc-error>"), std::string::npos);
  EXPECT_NE(response.xml.find("<error-tag>invalid-value</error-tag>"),
            std::string::npos);
  EXPECT_NE(response.xml.find("invalid edit-config parameters"),
            std::string::npos);
  EXPECT_NE(loaded.application->working_configuration().ToXml().find(
                ">edge-1</"),
            std::string::npos);
  EXPECT_TRUE(loaded.application->DrainBackendDeltas().empty());
}

}  // namespace
}  // namespace dangd
