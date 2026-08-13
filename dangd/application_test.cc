// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/application.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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
          .configuration = inputs.Write("config.xml", kConfig)};
}

TEST(DangdApplicationTest, LoadsModelAndCompleteConfiguration) {
  TemporaryInputs inputs;
  auto loaded = Application::Load(Options(inputs));
  ASSERT_NE(loaded.application, nullptr);
  EXPECT_TRUE(loaded.errors.empty());
  EXPECT_EQ(loaded.application->schema().roots().size(), 1u);
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
                "<error-path>/{urn:example:appliance}system/"
                "{urn:example:appliance}hostname</error-path>"),
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
