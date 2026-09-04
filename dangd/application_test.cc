// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/application.h"
#include "dangd/test_plugins/plugin_test_support.h"
#include "yang/xml_security.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <thread>
#include <vector>

#include <dlfcn.h>

#include <gtest/gtest.h>

namespace dangd {
namespace {

TEST(DangdOperationalDataTest, UsesBackendAppliedConfigurationAsOperational) {
  yang::netconf::NacmPolicy nacm;
  DangdOperationalData operational(
      "<yang-library><module-set/><content-id>test</content-id>"
      "</yang-library>",
      {}, &nacm, nullptr, nullptr);
  operational.SetAppliedConfigurationProvider([] {
    return "<data><applied xmlns='urn:test'>device</applied></data>";
  });
  const auto augmented = operational.AugmentDataXml(
      "<data><intended xmlns='urn:test'>server</intended></data>");
  const std::string& result = augmented.xml;
  EXPECT_NE(result.find("<applied"), std::string::npos) << result;
  EXPECT_EQ(result.find("<intended"), std::string::npos) << result;
}

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

unsigned int NmdaStressRequestsPerThread() {
  constexpr unsigned int kDefault = 25;
  constexpr unsigned int kMaximum = 100000;
  const char* configured = std::getenv("DANG_NMDA_STRESS_REQUESTS_PER_THREAD");
  if (!configured || !*configured) return kDefault;
  const std::string_view text(configured);
  unsigned int value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value == 0 || value > kMaximum)
    return kDefault;
  return value;
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
  EXPECT_NE(library.xml.find("<name>dangd-reconciliation</name>"),
            std::string::npos)
      << library.xml;
  EXPECT_NE(library.xml.find("<name>ietf-ssh-common</name>"),
            std::string::npos) << library.xml;
  EXPECT_NE(library.xml.find("<name>ietf-ssh-client</name>"),
            std::string::npos) << library.xml;
  EXPECT_NE(library.xml.find("<name>ietf-ssh-server</name>"),
            std::string::npos) << library.xml;
  EXPECT_NE(library.xml.find("<feature>xpath</feature>"), std::string::npos)
      << library.xml;
}

TEST(DangdApplicationTest, ManagesNacmProtectedCentralSymmetricKeys) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.state_file = inputs.Path("keystore-state.json");
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);

  yang::netconf::RpcSessionContext recovery{1, "alice", "alice", {}};
  const auto edit = loaded.application->server().Process(recovery, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="edit">
      <edit-config><target><candidate/></target><config>
        <keystore xmlns="urn:ietf:params:xml:ns:yang:ietf-keystore"
                  xmlns:ct="urn:ietf:params:xml:ns:yang:ietf-crypto-types">
          <symmetric-keys><symmetric-key>
            <name>backup-key</name>
            <key-format>ct:octet-string-key-format</key-format>
            <cleartext-symmetric-key>AQIDBA==</cleartext-symmetric-key>
          </symmetric-key></symmetric-keys>
        </keystore>
      </config></edit-config>
    </rpc>)xml");
  ASSERT_NE(edit.xml.find("<ok/>"), std::string::npos) << edit.xml;
  const auto committed = Commit(*loaded.application);
  ASSERT_NE(committed.xml.find("<ok/>"), std::string::npos) << committed.xml;

  const auto privileged = loaded.application->server().Process(recovery, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="read">
      <get-config><source><running/></source></get-config>
    </rpc>)xml");
  EXPECT_NE(privileged.xml.find("<name>backup-key</name>"), std::string::npos)
      << privileged.xml;
  EXPECT_NE(privileged.xml.find("AQIDBA=="), std::string::npos)
      << privileged.xml;

  yang::netconf::RpcSessionContext ordinary{2, "bob", "bob", {}};
  const auto filtered = loaded.application->server().Process(ordinary, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="read">
      <get-config><source><running/></source></get-config>
    </rpc>)xml");
  EXPECT_NE(filtered.xml.find("<name>backup-key</name>"), std::string::npos)
      << filtered.xml;
  EXPECT_EQ(filtered.xml.find("AQIDBA=="), std::string::npos) << filtered.xml;
  EXPECT_EQ(filtered.xml.find("cleartext-symmetric-key"), std::string::npos)
      << filtered.xml;

  const auto denied = loaded.application->server().Process(ordinary, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="denied">
      <edit-config><target><candidate/></target><config>
        <keystore xmlns="urn:ietf:params:xml:ns:yang:ietf-keystore"
                  xmlns:ct="urn:ietf:params:xml:ns:yang:ietf-crypto-types">
          <symmetric-keys><symmetric-key><name>intruder-key</name>
            <key-format>ct:octet-string-key-format</key-format>
            <cleartext-symmetric-key>AQ==</cleartext-symmetric-key>
          </symmetric-key></symmetric-keys>
        </keystore>
      </config></edit-config>
    </rpc>)xml");
  EXPECT_NE(denied.xml.find("access-denied"), std::string::npos) << denied.xml;

  const auto library = loaded.application->server().Process(recovery, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="library">
      <get/>
    </rpc>)xml");
  EXPECT_NE(library.xml.find("<name>ietf-keystore</name>"), std::string::npos)
      << library.xml;
  EXPECT_NE(library.xml.find("<feature>central-keystore-supported</feature>"),
            std::string::npos) << library.xml;
  EXPECT_NE(library.xml.find("<feature>symmetric-keys</feature>"),
            std::string::npos) << library.xml;

  loaded.application.reset();
  auto restored = Application::Load(options);
  ASSERT_NE(restored.application, nullptr)
      << testing::PrintToString(restored.errors);
  const auto after_restart = restored.application->server().Process(
      recovery, R"xml(
        <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="restored">
          <get-config><source><running/></source></get-config>
        </rpc>)xml");
  EXPECT_NE(after_restart.xml.find("<name>backup-key</name>"),
            std::string::npos) << after_restart.xml;
  EXPECT_NE(after_restart.xml.find("AQIDBA=="), std::string::npos)
      << after_restart.xml;
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
  EXPECT_NE(origins.xml.find("<hostname or:origin=\"or:intended\">edge-1"),
            std::string::npos)
      << origins.xml;

  const auto mixed_origins = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="mixed-origins"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><with-origin/>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(mixed_origins.xml.find("denied-operations or:origin"),
            std::string::npos)
      << mixed_origins.xml;

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

  const auto derived_origin = loaded.application->server().Process(
      session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="derived-origin"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:or="urn:ietf:params:xml:ns:yang:ietf-origin">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <origin-filter>or:unknown</origin-filter>
        <origin-filter>or:origin</origin-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(derived_origin.xml.find("edge-1"), std::string::npos)
      << derived_origin.xml;

  const auto negated_base = loaded.application->server().Process(
      session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="negated-base"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:or="urn:ietf:params:xml:ns:yang:ietf-origin">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <negated-origin-filter>or:origin</negated-origin-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(negated_base.xml.find("edge-1"), std::string::npos)
      << negated_base.xml;
  EXPECT_NE(negated_base.xml.find("yang-library"), std::string::npos)
      << negated_base.xml;

  const auto invalid_origin = loaded.application->server().Process(
      session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="bad-origin"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:bad="urn:example:appliance">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <origin-filter>bad:not-an-origin</origin-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(invalid_origin.xml.find("invalid-value"), std::string::npos)
      << invalid_origin.xml;
  EXPECT_NE(invalid_origin.xml.find("not derived from ietf-origin:origin"),
            std::string::npos)
      << invalid_origin.xml;

  const auto conflicting_origins = loaded.application->server().Process(
      session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="conflict"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:or="urn:ietf:params:xml:ns:yang:ietf-origin">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <origin-filter>or:intended</origin-filter>
        <negated-origin-filter>or:unknown</negated-origin-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(conflicting_origins.xml.find("invalid-value"), std::string::npos)
      << conflicting_origins.xml;

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

TEST(DangdApplicationTest, ValidatesCompleteNmdaOperationInput) {
  TemporaryInputs inputs;
  auto loaded = Application::Load(Options(inputs));
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};

  const auto duplicate = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="duplicate"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:running</datastore>
        <datastore>ds:candidate</datastore>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(duplicate.xml.find("invalid element count"), std::string::npos)
      << duplicate.xml;
  EXPECT_NE(duplicate.xml.find("<error-tag>invalid-value</error-tag>"),
            std::string::npos) << duplicate.xml;

  const auto unknown = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="unknown"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <edit-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:candidate</datastore>
        <unsupported/>
        <config><system xmlns="urn:example:appliance">
          <hostname>must-not-apply</hostname>
        </system></config>
      </edit-data>
    </rpc>)xml");
  EXPECT_NE(unknown.xml.find("<error-tag>unknown-element</error-tag>"),
            std::string::npos) << unknown.xml;
  EXPECT_EQ(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kCandidate)
                .ToXml()
                .find("must-not-apply"),
            std::string::npos);

  const auto unsupported_defaults = loaded.application->server().Process(
      session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="defaults"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:wd="urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:running</datastore>
        <wd:with-defaults>report-all</wd:with-defaults>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(unsupported_defaults.xml.find(
                "<error-tag>invalid-value</error-tag>"),
            std::string::npos) << unsupported_defaults.xml;
  EXPECT_NE(unsupported_defaults.xml.find("with-defaults is not supported"),
            std::string::npos) << unsupported_defaults.xml;
}

TEST(DangdApplicationTest, CoversConventionalNmdaRetrievalCrossProduct) {
  TemporaryInputs inputs;
  auto loaded = Application::Load(Options(inputs));
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  for (const std::string_view datastore :
       {"running", "candidate", "startup", "intended"}) {
    const std::string prefix =
        "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
        "message-id=\"matrix\" "
        "xmlns:ds=\"urn:ietf:params:xml:ns:yang:ietf-datastores\">"
        "<get-data xmlns=\"urn:ietf:params:xml:ns:yang:ietf-netconf-nmda\">"
        "<datastore>ds:" + std::string(datastore) + "</datastore>";
    const auto subtree = loaded.application->server().Process(
        session, prefix +
            "<subtree-filter><system xmlns=\"urn:example:appliance\"/>"
            "</subtree-filter><config-filter>true</config-filter>"
            "<max-depth>2</max-depth></get-data></rpc>");
    EXPECT_NE(subtree.xml.find("edge-1"), std::string::npos)
        << datastore << ": " << subtree.xml;

    const auto xpath = loaded.application->server().Process(
        session, prefix +
            "<xpath-filter xmlns:a=\"urn:example:appliance\">"
            "/a:system/a:hostname</xpath-filter><config-filter>true</config-filter>"
            "<max-depth>1</max-depth></get-data></rpc>");
    EXPECT_NE(xpath.xml.find("edge-1"), std::string::npos)
        << datastore << ": " << xpath.xml;

    const auto state_only = loaded.application->server().Process(
        session, prefix +
            "<config-filter>false</config-filter></get-data></rpc>");
    EXPECT_EQ(state_only.xml.find("edge-1"), std::string::npos)
        << datastore << ": " << state_only.xml;

    const auto origin = loaded.application->server().Process(
        session, prefix + "<with-origin/></get-data></rpc>");
    EXPECT_NE(origin.xml.find("<error-tag>invalid-value</error-tag>"),
              std::string::npos) << datastore << ": " << origin.xml;
  }

  const auto unknown = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="unknown"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:factory-default</datastore>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(unknown.xml.find("<error-tag>invalid-value</error-tag>"),
            std::string::npos) << unknown.xml;
}

TEST(DangdApplicationTest, AppliesNmdaDefaultOperationsAndLocks) {
  TemporaryInputs inputs;
  constexpr std::string_view model = R"yang(
    module appliance {
      yang-version 1.1;
      namespace "urn:example:appliance";
      prefix a;
      container system {
        leaf hostname { type string; mandatory true; }
        leaf location { type string; }
      }
    }
  )yang";
  constexpr std::string_view configuration = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:example:appliance">
        <hostname>edge-1</hostname><location>rack-1</location>
      </system>
    </config>
  )xml";
  auto options = Options(inputs);
  options.model = inputs.Write("operations.yang", model);
  options.configuration = inputs.Write("operations.xml", configuration);
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext alice{1, "alice", "alice", {}};
  yang::netconf::RpcSessionContext other{2, "alice", "other", {}};

  const auto edit = [&](const yang::netconf::RpcSessionContext& session,
                        std::string_view operation,
                        std::string_view hostname) {
    return loaded.application->server().Process(
        session,
        "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
        "message-id=\"edit\" xmlns:ds=\"urn:ietf:params:xml:ns:yang:"
        "ietf-datastores\"><edit-data xmlns=\"urn:ietf:params:xml:ns:yang:"
        "ietf-netconf-nmda\"><datastore>ds:candidate</datastore>"
        "<default-operation>" + std::string(operation) +
        "</default-operation><config><system "
        "xmlns=\"urn:example:appliance\"><hostname>" +
        std::string(hostname) + "</hostname></system></config>"
        "</edit-data></rpc>");
  };

  EXPECT_NE(edit(alice, "merge", "edge-merge").xml.find("<ok/>"),
            std::string::npos);
  std::string candidate = loaded.application->datastores()
                              .Read(yang::netconf::Datastore::kCandidate)
                              .ToXml();
  EXPECT_NE(candidate.find("edge-merge"), std::string::npos) << candidate;
  EXPECT_NE(candidate.find("rack-1"), std::string::npos) << candidate;

  EXPECT_NE(edit(alice, "replace", "edge-replace").xml.find("<ok/>"),
            std::string::npos);
  candidate = loaded.application->datastores()
                  .Read(yang::netconf::Datastore::kCandidate)
                  .ToXml();
  EXPECT_NE(candidate.find("edge-replace"), std::string::npos) << candidate;
  EXPECT_EQ(candidate.find("rack-1"), std::string::npos) << candidate;

  EXPECT_NE(edit(alice, "none", "ignored").xml.find("<ok/>"),
            std::string::npos);
  candidate = loaded.application->datastores()
                  .Read(yang::netconf::Datastore::kCandidate)
                  .ToXml();
  EXPECT_NE(candidate.find("edge-replace"), std::string::npos) << candidate;
  EXPECT_EQ(candidate.find("ignored"), std::string::npos) << candidate;

  const auto invalid = loaded.application->server().Process(alice, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="rollback"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <edit-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:candidate</datastore>
        <config><system xmlns="urn:example:appliance"
                        xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0">
          <location>must-rollback</location>
          <hostname nc:operation="delete"/>
        </system></config>
      </edit-data>
    </rpc>)xml");
  EXPECT_NE(invalid.xml.find("<rpc-error>"), std::string::npos) << invalid.xml;
  candidate = loaded.application->datastores()
                  .Read(yang::netconf::Datastore::kCandidate)
                  .ToXml();
  EXPECT_NE(candidate.find("edge-replace"), std::string::npos) << candidate;
  EXPECT_EQ(candidate.find("must-rollback"), std::string::npos) << candidate;

  const auto lock = loaded.application->server().Process(alice, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="lock"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:nmda="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
      <lock><target><nmda:datastore>ds:candidate</nmda:datastore></target></lock>
    </rpc>)xml");
  EXPECT_NE(lock.xml.find("<ok/>"), std::string::npos) << lock.xml;
  EXPECT_NE(edit(other, "merge", "locked-out").xml.find("lock-denied"),
            std::string::npos);

  const auto wrong_unlock = loaded.application->server().Process(other, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="unlock"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:nmda="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
      <unlock><target><nmda:datastore>ds:candidate</nmda:datastore></target></unlock>
    </rpc>)xml");
  EXPECT_NE(wrong_unlock.xml.find("lock-denied"), std::string::npos)
      << wrong_unlock.xml;

  const auto unlock = loaded.application->server().Process(alice, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="unlock"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:nmda="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
      <unlock><target><nmda:datastore>ds:candidate</nmda:datastore></target></unlock>
    </rpc>)xml");
  EXPECT_NE(unlock.xml.find("<ok/>"), std::string::npos) << unlock.xml;
  EXPECT_NE(edit(other, "merge", "after-unlock").xml.find("<ok/>"),
            std::string::npos);

  const auto readonly_lock = loaded.application->server().Process(alice, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="readonly"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:nmda="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
      <lock><target><nmda:datastore>ds:operational</nmda:datastore></target></lock>
    </rpc>)xml");
  EXPECT_NE(readonly_lock.xml.find("<error-tag>invalid-value</error-tag>"),
            std::string::npos) << readonly_lock.xml;
}

TEST(DangdApplicationTest, ComposesNmdaReadFiltersWithNacm) {
  TemporaryInputs inputs;
  constexpr std::string_view model = R"yang(
    module appliance {
      yang-version 1.1;
      namespace "urn:example:appliance";
      prefix a;
      container system {
        leaf hostname { type string; mandatory true; }
        leaf secret { type string; }
        container rack { leaf aisle { type string; } }
      }
    }
  )yang";
  constexpr std::string_view configuration = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:example:appliance">
        <hostname>edge-1</hostname><secret>hidden</secret>
        <rack><aisle>seven</aisle></rack>
      </system>
    </config>
  )xml";
  auto options = Options(inputs);
  options.model = inputs.Write("filters.yang", model);
  options.configuration = inputs.Write("filters.xml", configuration);
  options.recovery_users.clear();
  options.nacm_configuration = inputs.Write("filters-nacm.xml", R"xml(
    <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm"
          xmlns:a="urn:example:appliance">
      <read-default>permit</read-default><write-default>deny</write-default>
      <exec-default>deny</exec-default>
      <groups><group><name>readers</name><user-name>alice</user-name>
      </group></groups>
      <rule-list><name>read-policy</name><group>readers</group>
        <rule><name>allow-nmda</name><module-name>ietf-netconf-nmda</module-name>
          <rpc-name>get-data</rpc-name><access-operations>exec</access-operations>
          <action>permit</action></rule>
        <rule><name>hide-secret</name>
          <path>/a:system/a:secret</path>
          <access-operations>read</access-operations><action>deny</action>
        </rule>
      </rule-list>
    </nacm>)xml");
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext alice{1, "alice", "alice", {}};

  const auto response = loaded.application->server().Process(alice, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="combined"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <subtree-filter><system xmlns="urn:example:appliance"/></subtree-filter>
        <config-filter>true</config-filter>
        <max-depth>2</max-depth><with-origin/>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find("<rpc-error>"), std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("<hostname or:origin=\"or:intended\">edge-1"),
            std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("<rack or:origin=\"or:intended\""),
            std::string::npos) << response.xml;
  EXPECT_EQ(response.xml.find("hidden"), std::string::npos) << response.xml;
  EXPECT_EQ(response.xml.find("seven"), std::string::npos) << response.xml;
  EXPECT_EQ(response.xml.find("yang-library"), std::string::npos)
      << response.xml;

  const auto nested_subtree = loaded.application->server().Process(alice, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="subtree-depth"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <subtree-filter><system xmlns="urn:example:appliance">
          <rack/>
        </system></subtree-filter>
        <config-filter>true</config-filter><max-depth>1</max-depth>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(nested_subtree.xml.find("<rpc-error>"), std::string::npos)
      << nested_subtree.xml;
  EXPECT_NE(nested_subtree.xml.find("<system"), std::string::npos)
      << nested_subtree.xml;
  EXPECT_NE(nested_subtree.xml.find("<rack"), std::string::npos)
      << nested_subtree.xml;
  EXPECT_EQ(nested_subtree.xml.find("seven"), std::string::npos)
      << nested_subtree.xml;
  EXPECT_EQ(nested_subtree.xml.find("edge-1"), std::string::npos)
      << nested_subtree.xml;

  const auto xpath = loaded.application->server().Process(alice, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="xpath-depth"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:a="urn:example:appliance">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <xpath-filter>/a:system/a:rack</xpath-filter>
        <config-filter>true</config-filter><max-depth>1</max-depth>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(xpath.xml.find("<rpc-error>"), std::string::npos) << xpath.xml;
  EXPECT_NE(xpath.xml.find("<a:system"), std::string::npos) << xpath.xml;
  EXPECT_NE(xpath.xml.find("<a:rack"), std::string::npos) << xpath.xml;
  EXPECT_EQ(xpath.xml.find("seven"), std::string::npos) << xpath.xml;
  EXPECT_EQ(xpath.xml.find("edge-1"), std::string::npos) << xpath.xml;
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

TEST(DangdApplicationTest, ProvidesRemovableDangdOnlySuperuser) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.recovery_users.clear();
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr)
      << testing::PrintToString(loaded.errors);
  const std::string edit = R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="bootstrap">
      <edit-config><target><candidate/></target><config>
        <system xmlns="urn:example:appliance"><hostname>recovered</hostname></system>
      </config></edit-config>
    </rpc>)xml";
  EXPECT_NE(loaded.application->server().Process("guest", edit).xml.find(
                "access-denied"),
            std::string::npos);
  EXPECT_NE(loaded.application->server()
                .Process(std::string(kDefaultSuperuser), edit)
                .xml.find("<ok/>"),
            std::string::npos);
  const auto audit = loaded.application->DrainRecoveryAuditRecords();
  ASSERT_EQ(audit.size(), 1U);
  EXPECT_NE(audit.front().find("user=dangd-superuser"), std::string::npos);

  options.default_superuser = false;
  auto disabled = Application::Load(options);
  ASSERT_NE(disabled.application, nullptr)
      << testing::PrintToString(disabled.errors);
  EXPECT_NE(disabled.application->server()
                .Process(std::string(kDefaultSuperuser), edit)
                .xml.find("access-denied"),
            std::string::npos);
  EXPECT_TRUE(disabled.application->DrainRecoveryAuditRecords().empty());
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
  options.recovery_users = {"user=name"};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr)
      << testing::PrintToString(loaded.errors);
  const std::string request =
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"audit\"><get/></rpc>";
  (void)loaded.application->server().Process(
      {71, "ordinary", "ordinary", {}}, request);
  (void)loaded.application->server().Process(
      {72, "user=name", "user=name", {}}, request);

  const std::vector<std::string> records =
      loaded.application->DrainRecoveryAuditRecords();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_NE(records.front().find("session=72"), std::string::npos);
  EXPECT_NE(records.front().find("user=user%3Dname"), std::string::npos);
  EXPECT_EQ(records.front().find('\n'), std::string::npos);
  EXPECT_TRUE(loaded.application->DrainRecoveryAuditRecords().empty());
}

TEST(DangdApplicationTest, RejectsUnsafeOrDuplicateRecoveryUsers) {
  const std::vector<std::vector<std::string>> invalid = {
      {"alice", "alice"},
      {std::string(kDefaultSuperuser)},
      {" alice"},
      {"alice "},
      {"line\nuser"},
      {std::string("root\0admin", 10)},
      {std::string("bad\xff", 4)}};
  for (const auto& recovery_users : invalid) {
    TemporaryInputs inputs;
    auto options = Options(inputs);
    options.recovery_users = recovery_users;
    auto loaded = Application::Load(options);
    EXPECT_EQ(loaded.application, nullptr);
    ASSERT_FALSE(loaded.errors.empty());
    EXPECT_NE(loaded.errors.front().find("canonical UTF-8"), std::string::npos);
  }
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

TEST(DangdApplicationTest, ActivatesInitialAndRestoredPluginConfiguration) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.configuration = inputs.Write(
      "plugin-config.xml",
      "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<system xmlns=\"urn:example:appliance\"><hostname>edge-1</hostname>"
      "</system><provider-settings xmlns=\"urn:dangd:test:provider\">"
      "<mode>active</mode></provider-settings></config>");
  options.plugins = {DANG_TEST_CONSUMER_PLUGIN_PATH,
                     DANG_TEST_PROVIDER_PLUGIN_PATH};
  options.state_file = inputs.Path("plugin-state.json");
  const std::vector<std::string> expected{
      "provider.prepare", "consumer.prepare", "provider.validate",
      "consumer.validate", "provider.apply", "consumer.apply",
      "consumer.release", "provider.release"};

  test_plugin::ResetTrace();
  auto initial = Application::Load(options);
  ASSERT_NE(initial.application, nullptr)
      << testing::PrintToString(initial.errors);
  EXPECT_EQ(test_plugin::Trace(), expected);
  EXPECT_NE(test_plugin::Active("provider").find("<mode>active</mode>"),
            std::string::npos);

  test_plugin::ResetTrace();
  auto restored = Application::Load(options);
  ASSERT_NE(restored.application, nullptr)
      << testing::PrintToString(restored.errors);
  EXPECT_EQ(test_plugin::Trace(), expected);
  EXPECT_NE(test_plugin::Active("provider").find("<mode>active</mode>"),
            std::string::npos);
  EXPECT_TRUE(restored.application->DrainBackendDeltas().empty());
}

TEST(DangdApplicationTest, RejectsStartupWhenPluginHydrationFails) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.configuration = inputs.Write(
      "rejected-plugin-config.xml",
      "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<system xmlns=\"urn:example:appliance\"><hostname>edge-1</hostname>"
      "</system><provider-settings xmlns=\"urn:dangd:test:provider\">"
      "<mode>consumer-validate-fail</mode></provider-settings></config>");
  options.plugins = {DANG_TEST_CONSUMER_PLUGIN_PATH,
                     DANG_TEST_PROVIDER_PLUGIN_PATH};
  options.state_file = inputs.Path("rejected-state.json");

  test_plugin::ResetTrace();
  auto loaded = Application::Load(options);
  EXPECT_EQ(loaded.application, nullptr);
  ASSERT_FALSE(loaded.errors.empty());
  EXPECT_NE(loaded.errors.front().find("cannot activate startup configuration"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(*options.state_file));
  EXPECT_EQ(test_plugin::Trace(),
            (std::vector<std::string>{
                "provider.prepare", "consumer.prepare", "provider.validate",
                "consumer.validate", "consumer.release", "provider.release"}));
}

TEST(DangdApplicationTest,
     WorkerRuntimeLoadsModelAndRejectsPluginInvalidCommit) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_PLUGIN_PATH};
  options.plugin_worker_executable = DANG_TEST_PLUGIN_WORKER_PATH;
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

TEST(DangdApplicationTest, WorkerRejectsInvalidStartupPluginConfiguration) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.configuration = inputs.Write(
      "worker-startup.xml",
      "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<system xmlns=\"urn:example:appliance\"><hostname>edge-1</hostname>"
      "</system><plugin-settings xmlns=\"urn:dangd:example-plugin\">"
      "<mode>reject</mode></plugin-settings></config>");
  options.plugins = {DANG_TEST_PLUGIN_PATH};
  options.plugin_worker_executable = DANG_TEST_PLUGIN_WORKER_PATH;

  auto loaded = Application::Load(options);
  EXPECT_EQ(loaded.application, nullptr);
  ASSERT_FALSE(loaded.errors.empty());
  EXPECT_NE(loaded.errors.front().find("cannot activate startup configuration"),
            std::string::npos);
  EXPECT_NE(loaded.errors.front().find(
                "mode 'reject' is not supported by the reference plugin"),
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
  const auto address_action = actions.find("192.0.2.1");
  const auto activation_action = actions.find("}enabled with value");
  ASSERT_NE(address_action, std::string::npos) << actions;
  ASSERT_NE(activation_action, std::string::npos) << actions;
  EXPECT_LT(address_action, activation_action) << actions;
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

TEST(DangdApplicationTest, ReportsAndOmitsInvalidOperationalPluginData) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_BROKEN_OPERATIONAL_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="bad-state"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find(">invalid</counter>"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational provider test-broken-operational failed during validation"),
            std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("<error-tag>operation-failed</error-tag>"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("outside the YANG type&apos;s value space"),
            std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("urn:dangd:test:broken-operational}counter"),
            std::string::npos)
      << response.xml;

  const auto legacy_response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="bad-state-get">
      <get/>
    </rpc>)xml");
  EXPECT_EQ(legacy_response.xml.find(">invalid</counter>"), std::string::npos)
      << legacy_response.xml;
  EXPECT_NE(legacy_response.xml.find(
                "operational provider test-broken-operational failed during validation"),
            std::string::npos)
      << legacy_response.xml;
  EXPECT_NE(legacy_response.xml.find("operational-provider-failure"),
            std::string::npos)
      << legacy_response.xml;
}

TEST(DangdApplicationTest, RejectsOversizedOperationalProviderData) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_OVERSIZED_OPERATIONAL_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  const auto response = loaded.application->server().Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="oversized">
      <get/>
    </rpc>)xml");
  EXPECT_NE(response.xml.find("<error-tag>operation-failed</error-tag>"),
            std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("test-broken-operational failed during callback"),
            std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("operational XML exceeds the resource limit"),
            std::string::npos) << response.xml;
  EXPECT_EQ(response.xml.find(std::string(1024, 'x')), std::string::npos)
      << response.xml;
}

TEST(DangdApplicationTest, SustainsConcurrentOperationalProviderRetrieval) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_CONCURRENT_OPERATIONAL_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);

  constexpr unsigned int kThreads = 8;
  const unsigned int requests_per_thread = NmdaStressRequestsPerThread();
  std::atomic<unsigned int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<unsigned int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (unsigned int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&, thread] {
      ready.fetch_add(1);
      while (!start.load()) std::this_thread::yield();
      for (unsigned int request = 0; request < requests_per_thread; ++request) {
        yang::netconf::RpcSessionContext session{
            1000 + thread, "alice", "alice", {}};
        const auto response = loaded.application->server().Process(session,
            "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
            "message-id=\"concurrent\"><get/></rpc>");
        if (response.xml.find("<rpc-reply") == std::string::npos ||
            response.xml.find("<rpc-error>") != std::string::npos ||
            response.xml.find("<maximum-concurrency>") == std::string::npos)
          failures.fetch_add(1);
      }
    });
  }
  while (ready.load() != kThreads) std::this_thread::yield();
  start.store(true);
  for (std::thread& worker : workers) worker.join();

  EXPECT_EQ(failures.load(), 0U);
  const auto final = loaded.application->server().Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="final">
      <get/>
    </rpc>)xml");
  EXPECT_NE(final.xml.find("<maximum-concurrency>8</maximum-concurrency>"),
            std::string::npos) << final.xml;
}

TEST(DangdApplicationTest, EnforcesCompleteProviderChildCollections) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_COMPLETE_OPERATIONAL_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="complete"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find("<target-ref>missing</target-ref>"),
            std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational provider test-complete-operational failed during validation"),
            std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational-provider-failure"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("leafref value has no matching target instance"),
            std::string::npos)
      << response.xml;
}

TEST(DangdApplicationTest, RejectsMissingMandatoryNodeFromSeparateProvider) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_MANDATORY_OWNER_PLUGIN_PATH,
                     DANG_TEST_MANDATORY_PUBLISHER_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="mandatory"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find("<name>uplink</name>"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational provider test-mandatory-publisher failed during validation"),
            std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("operational-provider-failure"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("mandatory data node is absent"),
            std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("operational-mandatory}status"),
            std::string::npos) << response.xml;
}

TEST(DangdApplicationTest, KeepsCompletenessScopedToExactListInstance) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_MANDATORY_OWNER_PLUGIN_PATH,
                     DANG_TEST_MANDATORY_COMPLETE_PLUGIN_PATH,
                     DANG_TEST_MANDATORY_PARTIAL_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  const auto response = loaded.application->server().Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="scoped">
      <get/>
    </rpc>)xml");
  EXPECT_NE(response.xml.find("<name>uplink</name>"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("<name>wan</name>"), std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find("operational-provider-failure"),
            std::string::npos) << response.xml;
}

TEST(DangdApplicationTest, ResolvesStateLeafrefAcrossOperationalProviders) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_LEAFREF_OWNER_PLUGIN_PATH,
                     DANG_TEST_LEAFREF_TARGET_PLUGIN_PATH,
                     DANG_TEST_LEAFREF_VALID_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="leafref-ok"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(response.xml.find("<name>present</name>"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("<selected>present</selected>"), std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find("<provider>test-leafref-valid</provider>"),
            std::string::npos) << response.xml;
}

TEST(DangdApplicationTest, RejectsUnresolvedStateLeafrefFromLaterProvider) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_LEAFREF_OWNER_PLUGIN_PATH,
                     DANG_TEST_LEAFREF_TARGET_PLUGIN_PATH,
                     DANG_TEST_LEAFREF_INVALID_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="leafref-bad"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find("<name>present</name>"), std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find("<selected>missing</selected>"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational provider test-leafref-invalid failed during merge"),
            std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("operational-provider-failure"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("leafref value has no matching target instance"),
            std::string::npos) << response.xml;
}

TEST(DangdApplicationTest, ResolvesInstanceIdentifierAcrossStateProviders) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_INSTANCE_OWNER_PLUGIN_PATH,
                     DANG_TEST_INSTANCE_TARGET_PLUGIN_PATH,
                     DANG_TEST_INSTANCE_VALID_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="instance-ok"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(response.xml.find("oi:name='present'"), std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find("<provider>test-instance-valid</provider>"),
            std::string::npos) << response.xml;
}

TEST(DangdApplicationTest, RejectsInstanceIdentifierIntoClosedStateSubtree) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_INSTANCE_OWNER_PLUGIN_PATH,
                     DANG_TEST_INSTANCE_TARGET_PLUGIN_PATH,
                     DANG_TEST_INSTANCE_INVALID_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="instance-bad"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find("oi:name='missing'"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational provider test-instance-invalid failed during merge"),
            std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("operational-provider-failure"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find(
                "instance-identifier does not select an existing data node"),
            std::string::npos) << response.xml;
}

TEST(DangdApplicationTest, RejectsLaterOperationalProviderCollision) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.configuration = inputs.Write("collision.xml", R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:example:appliance"><hostname>edge-1</hostname></system>
      <configured-counter xmlns="urn:dangd:test:operational-collision">1</configured-counter>
    </config>
  )xml");
  options.plugins = {DANG_TEST_COLLISION_FIRST_PLUGIN_PATH,
                     DANG_TEST_COLLISION_SECOND_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="collision"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find(
                "<counter xmlns=\"urn:dangd:test:operational-collision\">1"
                "</counter>"),
            std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find(
                "<counter xmlns=\"urn:dangd:test:operational-collision\">2"
                "</counter>"),
            std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find(
                "operational provider test-collision-second failed during merge"),
            std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational-provider-failure"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("occurs more than once"), std::string::npos)
      << response.xml;
}

TEST(DangdApplicationTest, RejectsOperationalReferenceMissingFromAppliedData) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.configuration = inputs.Write("collision.xml", R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:example:appliance"><hostname>edge-1</hostname></system>
      <configured-counter xmlns="urn:dangd:test:operational-collision">2</configured-counter>
    </config>
  )xml");
  options.plugins = {DANG_TEST_COLLISION_FIRST_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="reference"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find(
                "<counter xmlns=\"urn:dangd:test:operational-collision\">"),
            std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find(
                "operational provider test-collision-first failed during merge"),
            std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational-provider-failure"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("leafref value has no matching target instance"),
            std::string::npos)
      << response.xml;
}

TEST(DangdApplicationTest, RejectsOperationalProviderCollisionWithCoreData) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_COLLISION_CORE_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="core-collision"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find("<netconf-state"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find(
                "operational provider test-collision-core failed during merge"),
            std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational-provider-failure"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("occurs more than once"), std::string::npos)
      << response.xml;
}

TEST(DangdApplicationTest, RejectsCrossProviderUniqueConstraintViolation) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_UNIQUE_FIRST_PLUGIN_PATH,
                     DANG_TEST_UNIQUE_SECOND_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="unique"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find("<name>first</name>"), std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find("<name>second</name>"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find(
                "operational provider test-unique-second failed during merge"),
            std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational-provider-failure"), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find(
                "list entries have identical values for a unique constraint"),
            std::string::npos)
      << response.xml;
}

class OperationalXPathConstraintTest
    : public testing::TestWithParam<std::tuple<const char*, const char*,
                                               const char*>> {};

TEST_P(OperationalXPathConstraintTest,
       RejectsConstraintResolvedByEarlierProvider) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_XPATH_CONTEXT_PLUGIN_PATH,
                     std::get<0>(GetParam())};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="xpath"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find(
                "<mode xmlns=\"urn:dangd:test:operational-xpath\">blocked"),
            std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find(std::get<1>(GetParam())), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find(std::get<2>(GetParam())), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational-provider-failure"), std::string::npos)
      << response.xml;
}

INSTANTIATE_TEST_SUITE_P(
    MustAndWhen, OperationalXPathConstraintTest,
    testing::Values(
        std::tuple{DANG_TEST_XPATH_MUST_PLUGIN_PATH, "<must-state",
                   "must constraint evaluates to false"},
        std::tuple{DANG_TEST_XPATH_WHEN_PLUGIN_PATH, "<when-state",
                   "when constraint evaluates to false"}),
    [](const testing::TestParamInfo<OperationalXPathConstraintTest::ParamType>&
           info) { return info.index == 0 ? "Must" : "When"; });

class OperationalXPathAbsenceTest
    : public testing::TestWithParam<std::tuple<const char*, const char*,
                                               const char*>> {};

TEST_P(OperationalXPathAbsenceTest, UsesEarlierCompleteSubtreeToDecideAbsence) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_XPATH_ABSENCE_OWNER_PLUGIN_PATH,
                     DANG_TEST_XPATH_ABSENCE_INPUT_PLUGIN_PATH,
                     std::get<0>(GetParam())};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto response = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="xpath-absence"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(response.xml.find(
                "<inputs xmlns=\"urn:dangd:test:operational-xpath-absence\""),
            std::string::npos) << response.xml;
  EXPECT_EQ(response.xml.find(std::get<1>(GetParam())), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find(std::get<2>(GetParam())), std::string::npos)
      << response.xml;
  EXPECT_NE(response.xml.find("operational-provider-failure"), std::string::npos)
      << response.xml;
}

INSTANTIATE_TEST_SUITE_P(
    MustAndWhen, OperationalXPathAbsenceTest,
    testing::Values(
        std::tuple{DANG_TEST_XPATH_ABSENCE_MUST_PLUGIN_PATH, "<must-state",
                   "must constraint evaluates to false"},
        std::tuple{DANG_TEST_XPATH_ABSENCE_WHEN_PLUGIN_PATH, "<when-state",
                   "when constraint evaluates to false"}),
    [](const testing::TestParamInfo<OperationalXPathAbsenceTest::ParamType>&
           info) { return info.index == 0 ? "Must" : "When"; });

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

TEST(DangdApplicationTest, PublishesBackendAppliedStateAndNodeOutcomes) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_PROVIDER_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  ASSERT_NE(SetProviderMode(*loaded.application, "backend-transform")
                .xml.find("<ok/>"), std::string::npos);
  ASSERT_NE(Commit(*loaded.application).xml.find("<ok/>"), std::string::npos);

  const auto response = loaded.application->server().Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="state">
      <get/>
    </rpc>)xml");
  EXPECT_NE(response.xml.find("device-normalized"), std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find("backend-transform"), std::string::npos)
      << response.xml;
  EXPECT_EQ(response.xml.find("or:origin="), std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("<disposition>transformed</disposition>"),
            std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("<disposition>rejected</disposition>"),
            std::string::npos) << response.xml;
  EXPECT_NE(response.xml.find("<disposition>delayed</disposition>"),
            std::string::npos) << response.xml;

  const auto system = loaded.application->server().Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="system"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:or="urn:ietf:params:xml:ns:yang:ietf-origin">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <origin-filter>or:system</origin-filter><with-origin/>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(system.xml.find("device-normalized"), std::string::npos)
      << system.xml;
  EXPECT_NE(system.xml.find("or:origin=\"or:system\""), std::string::npos)
      << system.xml;

  const auto learned = loaded.application->server().Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="learned"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores"
         xmlns:or="urn:ietf:params:xml:ns:yang:ietf-origin">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore>
        <origin-filter>or:learned</origin-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_EQ(learned.xml.find("device-normalized"), std::string::npos)
      << learned.xml;
}

TEST(DangdApplicationTest, RejectsAndCompensatesInvalidAppliedStateReport) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_PROVIDER_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);
  test_plugin::ResetTrace();
  ASSERT_NE(SetProviderMode(*loaded.application, "backend-invalid-report")
                .xml.find("<ok/>"), std::string::npos);

  const auto commit = Commit(*loaded.application);
  EXPECT_NE(commit.xml.find("invalid applied state"), std::string::npos)
      << commit.xml;
  const auto trace = test_plugin::Trace();
  EXPECT_NE(std::find(trace.begin(), trace.end(), "provider.rollback"),
            trace.end());
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

TEST(DangdApplicationTest, RejectsExhaustedHardwareBeforeApplyingAnything) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_CONSUMER_PLUGIN_PATH,
                     DANG_TEST_PROVIDER_PLUGIN_PATH};
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr);
  test_plugin::ResetTrace();

  ASSERT_NE(SetProviderMode(*loaded.application, "resource-exhausted")
                .xml.find("<ok/>"),
            std::string::npos);
  const auto commit = Commit(*loaded.application);
  EXPECT_NE(commit.xml.find("simulated hardware capacity exhausted"),
            std::string::npos) << commit.xml;
  EXPECT_EQ(test_plugin::Trace(),
            (std::vector<std::string>{
                "provider.prepare", "consumer.prepare", "provider.validate",
                "consumer.validate", "consumer.release", "provider.release"}));
  EXPECT_TRUE(test_plugin::Active("provider").empty());
  EXPECT_TRUE(test_plugin::Active("consumer").empty());
  EXPECT_EQ(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kRunning)
                .ToXml()
                .find("resource-exhausted"),
            std::string::npos);
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
  EXPECT_NE(commit.xml.find("hardware-state-diverged"), std::string::npos)
      << commit.xml;
  EXPECT_NE(test_plugin::Active("provider").find(
                "consumer-apply-rollback-fail"),
            std::string::npos);
  EXPECT_EQ(loaded.application->datastores()
                .Read(yang::netconf::Datastore::kRunning)
                .ToXml()
                .find("consumer-apply-rollback-fail"),
            std::string::npos);

  yang::netconf::RpcSessionContext session{1, "alice", "alice", {}};
  const auto reconciliation = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="reconcile"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
        <subtree-filter><hardware-reconciliation
            xmlns="urn:dangd:reconciliation"/></subtree-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(reconciliation.xml.find("<diverged>true</diverged>"),
            std::string::npos)
      << reconciliation.xml;
  EXPECT_NE(reconciliation.xml.find("simulated provider rollback failure"),
            std::string::npos)
      << reconciliation.xml;
  EXPECT_NE(reconciliation.xml.find("<instance-path"), std::string::npos)
      << reconciliation.xml;

  ASSERT_NE(SetProviderMode(*loaded.application, "active").xml.find("<ok/>"),
            std::string::npos);
  ASSERT_NE(Commit(*loaded.application).xml.find("<ok/>"), std::string::npos);
  const auto reconciled = loaded.application->server().Process(
      session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="reconciled"
         xmlns:ds="urn:ietf:params:xml:ns:yang:ietf-datastores">
      <get-data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-nmda">
        <datastore>ds:operational</datastore><config-filter>false</config-filter>
        <subtree-filter><hardware-reconciliation
            xmlns="urn:dangd:reconciliation"/></subtree-filter>
      </get-data>
    </rpc>)xml");
  EXPECT_NE(reconciled.xml.find("<diverged>false</diverged>"),
            std::string::npos)
      << reconciled.xml;
  EXPECT_EQ(reconciled.xml.find("<remnant>"), std::string::npos)
      << reconciled.xml;
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
  const auto reconciliation = retrieve(
      "<identifier>dangd-reconciliation</identifier>"
      "<version>2026-08-23</version>");
  EXPECT_NE(reconciliation.xml.find("module dangd-reconciliation"),
            std::string::npos)
      << reconciliation.xml;
  const auto ssh_server = retrieve(
      "<identifier>ietf-ssh-server</identifier>"
      "<version>2024-10-10</version>");
  EXPECT_NE(ssh_server.xml.find("module ietf-ssh-server"),
            std::string::npos) << ssh_server.xml;
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
  ASSERT_TRUE(loaded.application->PublishYangLibraryUpdate(
      "x</content-id><injected/>&\"'"));
  const auto escaped =
      loaded.application->server().DrainNotifications(session.session_id);
  ASSERT_EQ(escaped.size(), 2u);
  for (const std::string& event : escaped) {
    EXPECT_EQ(event.find("<injected"), std::string::npos) << event;
    EXPECT_NE(event.find("&lt;/"), std::string::npos) << event;
    EXPECT_NE(event.find("&amp;"), std::string::npos) << event;
  }
  std::string embedded_nul = "identifier";
  embedded_nul.push_back('\0');
  embedded_nul += "hidden";
  EXPECT_FALSE(loaded.application->PublishYangLibraryUpdate(embedded_nul));
  EXPECT_TRUE(loaded.application->server()
                  .DrainNotifications(session.session_id)
                  .empty());
  EXPECT_TRUE(loaded.application->PublishYangLibraryUpdate(
      loaded.application->yang_library_content_id()));
  EXPECT_TRUE(loaded.application->server()
                  .DrainNotifications(session.session_id)
                  .empty());
}

TEST(DangdApplicationTest, ValidatesAndPublishesPluginNotifications) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  options.plugins = {DANG_TEST_PROVIDER_PLUGIN_PATH};
  options.plugin_worker_executable = DANG_TEST_PLUGIN_WORKER_PATH;
  options.nacm_configuration = inputs.Write("notification-nacm.xml", R"xml(
    <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
      <read-default>deny</read-default><write-default>deny</write-default>
      <exec-default>permit</exec-default>
    </nacm>)xml");
  auto loaded = Application::Load(options);
  ASSERT_NE(loaded.application, nullptr) << testing::PrintToString(loaded.errors);

  yang::netconf::RpcSessionContext session{43, "alice", "alice", {}};
  const auto subscribe = loaded.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="sub">
      <create-subscription
        xmlns="urn:ietf:params:xml:ns:netconf:notification:1.0"/>
    </rpc>)xml");
  ASSERT_NE(subscribe.xml.find("<ok/>"), std::string::npos) << subscribe.xml;
  yang::netconf::RpcSessionContext denied{44, "bob", "bob", {}};
  const auto denied_subscribe = loaded.application->server().Process(denied, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="denied-sub">
      <create-subscription
        xmlns="urn:ietf:params:xml:ns:netconf:notification:1.0"/>
    </rpc>)xml");
  ASSERT_NE(denied_subscribe.xml.find("<ok/>"), std::string::npos)
      << denied_subscribe.xml;

  EXPECT_TRUE(loaded.application->PollPluginNotifications().empty());
  const auto notifications =
      loaded.application->server().DrainNotifications(session.session_id);
  ASSERT_EQ(notifications.size(), 1u);
  EXPECT_NE(notifications.front().find("<provider-event"), std::string::npos)
      << notifications.front();
  EXPECT_TRUE(loaded.application->server()
                  .DrainNotifications(denied.session_id)
                  .empty());
  EXPECT_NE(notifications.front().find("<status>ready</status>"),
            std::string::npos)
      << notifications.front();
  EXPECT_TRUE(loaded.application->PollPluginNotifications().empty());
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

TEST(DangdApplicationTest, ReloadsLibraryInventoryAndAdvertisedDatastoreSchema) {
  TemporaryInputs inputs;
  auto options = Options(inputs);
  auto base = Application::Load(options);
  ASSERT_NE(base.application, nullptr) << testing::PrintToString(base.errors);
  yang::netconf::RpcSessionContext session{77, "alice", "alice", {}};
  ASSERT_NE(base.application->server().Process(session, R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="sub">
      <create-subscription
        xmlns="urn:ietf:params:xml:ns:netconf:notification:1.0"/>
    </rpc>)xml").xml.find("<ok/>"), std::string::npos);
  const std::string base_id = base.application->yang_library_content_id();

  options.plugins = {DANG_TEST_DEVIATION_PLUGIN_PATH};
  auto deviated = Application::Reload(options, *base.application);
  ASSERT_NE(deviated.application, nullptr)
      << testing::PrintToString(deviated.errors);
  const std::string deviation_id =
      deviated.application->yang_library_content_id();
  EXPECT_NE(deviation_id, base_id);
  ASSERT_TRUE(base.application->PublishYangLibraryUpdate(deviation_id));
  const auto updates =
      base.application->server().DrainNotifications(session.session_id);
  ASSERT_EQ(updates.size(), 2u);
  EXPECT_NE(updates[0].find("yang-library-update"), std::string::npos);
  EXPECT_NE(updates[1].find("yang-library-change"), std::string::npos);

  options.plugins = {DANG_TEST_PLUGIN_PATH};
  auto plugin = Application::Reload(options, *deviated.application);
  ASSERT_NE(plugin.application, nullptr) << testing::PrintToString(plugin.errors);
  EXPECT_NE(plugin.application->yang_library_content_id(), deviation_id);
  EXPECT_TRUE(plugin.application->schema()
                  .FindRoot({"urn:dangd:example-plugin", "plugin-settings"})
                  .has_value());

  const auto library = plugin.application->server().Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="library">
      <get/>
    </rpc>)xml");
  pugi::xml_document parsed;
  ASSERT_TRUE(yang::ParseUntrustedXml(library.xml, &parsed).ok) << library.xml;
  const pugi::xml_node library_node =
      parsed.document_element().child("data").child("yang-library");
  ASSERT_TRUE(library_node) << library.xml;
  std::size_t datastore_count = 0;
  for (const pugi::xml_node datastore : library_node.children("datastore")) {
    ++datastore_count;
    EXPECT_STREQ(datastore.child("schema").text().as_string(), "dangd-schema");
  }
  EXPECT_EQ(datastore_count, 5u);

  const auto source = plugin.application->server().Process("alice", R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="source">
      <get-schema xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring">
        <identifier>dangd-example-plugin</identifier>
        <version>2026-08-13</version>
      </get-schema>
    </rpc>)xml");
  EXPECT_NE(source.xml.find("module dangd-example-plugin"), std::string::npos)
      << source.xml;
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
