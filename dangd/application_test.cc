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

}  // namespace
}  // namespace dangd
