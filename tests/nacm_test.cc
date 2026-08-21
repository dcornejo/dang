// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/nacm.h"

namespace yang::netconf {
namespace {

TEST(NacmTest, AppliesOrderedGroupRulesAndDefaults) {
  NacmPolicy policy;
  policy.AddUserToGroup("alice", "operators");
  policy.AddRule({"allow-host", "operators", "",
                  "/{urn:example}system/{urn:example}hostname",
                  static_cast<std::uint8_t>(
                      AccessMask(AccessOperation::kRead) |
                      AccessMask(AccessOperation::kUpdate)),
                  AccessAction::kPermit});
  EXPECT_TRUE(policy.AuthorizeData(
      "alice", AccessOperation::kUpdate,
      "/{urn:example}system/{urn:example}hostname"));
  EXPECT_FALSE(policy.AuthorizeData(
      "alice", AccessOperation::kUpdate,
      "/{urn:example}system/{urn:example}password"));
  EXPECT_TRUE(policy.AuthorizeData(
      "alice", AccessOperation::kRead, "/{urn:example}public"));
}

TEST(NacmTest, DeniesSensitiveRpcByRuleAndDeleteConfigByDefault) {
  NacmPolicy policy;
  policy.AddUserToGroup("guest", "guests");
  policy.AddRule({"deny-commit", "guests", "commit", "",
                  AccessMask(AccessOperation::kExecute),
                  AccessAction::kDeny});
  EXPECT_FALSE(policy.AuthorizeRpc("guest", "commit"));
  EXPECT_TRUE(policy.AuthorizeRpc("guest", "get"));
  EXPECT_FALSE(policy.AuthorizeRpc("guest", "delete-config"));
  policy.set_exec_default(AccessAction::kDeny);
  EXPECT_TRUE(policy.AuthorizeRpc("guest", "close-session"));
}

TEST(NacmTest, RecoveryUsersBypassRules) {
  NacmPolicy policy;
  policy.set_read_default(AccessAction::kDeny);
  policy.set_write_default(AccessAction::kDeny);
  policy.set_exec_default(AccessAction::kDeny);
  policy.AddRecoveryUser("root");
  EXPECT_TRUE(policy.AuthorizeRpc("root", "delete-config"));
  EXPECT_TRUE(policy.AuthorizeData("root", AccessOperation::kDelete, "/any"));
}

TEST(NacmTest, DisabledEnforcementBypassesRulesWithoutCountingDenials) {
  NacmPolicy policy;
  policy.set_enabled(false);
  policy.set_read_default(AccessAction::kDeny);
  policy.set_write_default(AccessAction::kDeny);
  policy.set_exec_default(AccessAction::kDeny);
  EXPECT_TRUE(policy.AuthorizeRpc("guest", "ietf-netconf", "delete-config"));
  EXPECT_TRUE(policy.AuthorizeData(
      "guest", "example", AccessOperation::kCreate, "/{urn:example}item"));
  EXPECT_TRUE(policy.AuthorizeData(
      "guest", "example", AccessOperation::kUpdate, "/{urn:example}item"));
  EXPECT_TRUE(policy.AuthorizeData(
      "guest", "example", AccessOperation::kDelete, "/{urn:example}item"));
  EXPECT_TRUE(policy.AuthorizeNotification("guest", "example", "alarm"));
  const NacmCounters counters = policy.counters();
  EXPECT_EQ(counters.denied_operations, 0U);
  EXPECT_EQ(counters.denied_data_writes, 0U);
  EXPECT_EQ(counters.denied_notifications, 0U);
}

TEST(NacmTest, AppliesWildcardGroupsModulesNamesAndCrudxBitsInOrder) {
  NacmPolicy policy;
  policy.set_read_default(AccessAction::kDeny);
  policy.set_write_default(AccessAction::kDeny);
  policy.set_exec_default(AccessAction::kDeny);
  NacmRule first;
  first.name = "permit-all-group-example";
  first.groups = {"*"};
  first.module_name = "*";
  first.path_prefix = "/{urn:example}items";
  first.operations = AccessMask(AccessOperation::kRead) |
      AccessMask(AccessOperation::kCreate) |
      AccessMask(AccessOperation::kUpdate) |
      AccessMask(AccessOperation::kDelete);
  first.action = AccessAction::kPermit;
  policy.AddRule(first);
  NacmRule later_deny = first;
  later_deny.name = "later-deny";
  later_deny.action = AccessAction::kDeny;
  policy.AddRule(std::move(later_deny));
  for (const AccessOperation operation : {
           AccessOperation::kRead, AccessOperation::kCreate,
           AccessOperation::kUpdate, AccessOperation::kDelete}) {
    EXPECT_TRUE(policy.AuthorizeData(
        "unlisted", "example", operation,
        "/{urn:example}items/{urn:example}item"));
  }

  NacmRule rpc;
  rpc.name = "all-rpcs";
  rpc.groups = {"*"};
  rpc.module_name = "*";
  rpc.rpc_name = "*";
  rpc.operations = AccessMask(AccessOperation::kExecute);
  rpc.action = AccessAction::kPermit;
  policy.AddRule(std::move(rpc));
  EXPECT_TRUE(policy.AuthorizeRpc("unlisted", "vendor", "reboot"));
}

TEST(NacmTest, SilentlyFiltersDeniedReadSubtrees) {
  NacmPolicy policy;
  policy.AddUserToGroup("guest", "guests");
  policy.AddRule({"deny-secret", "guests", "",
                  "/{urn:example}system/{urn:example}secret",
                  AccessMask(AccessOperation::kRead), AccessAction::kDeny});
  const std::string filtered = policy.FilterReadableData("guest", R"xml(
    <data xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:example"><public>yes</public><secret>no</secret></system>
    </data>)xml");
  EXPECT_NE(filtered.find("<public>yes</public>"), std::string::npos);
  EXPECT_EQ(filtered.find("<secret>"), std::string::npos);
}

TEST(NacmTest, LoadsOrderedIetfNetconfAcmConfiguration) {
  const NacmLoadResult loaded = LoadNacmPolicy(R"xml(
    <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm"
          xmlns:if="urn:ietf:params:xml:ns:yang:ietf-interfaces">
      <read-default>deny</read-default>
      <write-default>deny</write-default>
      <exec-default>deny</exec-default>
      <groups><group><name>operators</name><user-name>alice</user-name>
      </group></groups>
      <rule-list><name>operator-rules</name><group>operators</group>
        <rule><name>deny-delete</name><module-name>ietf-netconf</module-name>
          <rpc-name>delete-config</rpc-name><access-operations>exec</access-operations>
          <action>deny</action></rule>
        <rule><name>allow-netconf</name><module-name>ietf-netconf</module-name>
          <rpc-name>*</rpc-name><access-operations>exec</access-operations>
          <action>permit</action></rule>
        <rule><name>allow-interfaces</name><module-name>*</module-name>
          <path>/if:interfaces</path>
          <access-operations>read update</access-operations>
          <action>permit</action></rule>
      </rule-list>
    </nacm>)xml");
  ASSERT_TRUE(loaded.errors.empty());
  ASSERT_TRUE(loaded.policy);
  EXPECT_FALSE(loaded.policy->AuthorizeRpc(
      "alice", "ietf-netconf", "delete-config"));
  EXPECT_TRUE(loaded.policy->AuthorizeRpc("alice", "ietf-netconf", "commit"));
  EXPECT_FALSE(loaded.policy->AuthorizeRpc("bob", "ietf-netconf", "commit"));
  EXPECT_TRUE(loaded.policy->AuthorizeData(
      "alice", "ietf-interfaces", AccessOperation::kUpdate,
      "/{urn:ietf:params:xml:ns:yang:ietf-interfaces}interfaces/"
      "{urn:ietf:params:xml:ns:yang:ietf-interfaces}interface"));
  EXPECT_FALSE(loaded.policy->AuthorizeData(
      "alice", "ietf-interfaces", AccessOperation::kDelete,
      "/{urn:ietf:params:xml:ns:yang:ietf-interfaces}interfaces"));
}

TEST(NacmTest, HonorsExternalGroupsSwitch) {
  const std::vector<std::string> transport_groups = {"operators"};
  NacmPolicy enabled;
  enabled.set_exec_default(AccessAction::kDeny);
  NacmRule rule{"allow", "", "commit", "",
                AccessMask(AccessOperation::kExecute), AccessAction::kPermit};
  rule.groups = {"operators"};
  enabled.AddRule(rule);
  EXPECT_TRUE(enabled.AuthorizeRpc("alice", "ietf-netconf", "commit",
                                   transport_groups));
  enabled.set_external_groups_enabled(false);
  EXPECT_FALSE(enabled.AuthorizeRpc("alice", "ietf-netconf", "commit",
                                    transport_groups));
}

TEST(NacmTest, RejectsMalformedModelConfiguration) {
  NacmLoadResult loaded = LoadNacmPolicy(R"xml(
    <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
      <enable-nacm>perhaps</enable-nacm>
      <rule-list><name>bad</name><group>users</group>
        <rule><name>bad-rule</name><access-operations>dance</access-operations>
          <action>maybe</action></rule>
      </rule-list>
    </nacm>)xml");
  EXPECT_FALSE(loaded.policy);
  EXPECT_GE(loaded.errors.size(), 3U);
}

TEST(NacmTest, RejectsInvalidModelShapeAndLeafListValues) {
  const std::vector<std::string> invalid = {
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
        <read-default>permit</read-default><read-default>deny</read-default>
      </nacm>)xml",
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
        <denied-operations>0</denied-operations>
      </nacm>)xml",
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
        <groups><group><name>*admins</name></group></groups>
      </nacm>)xml",
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
        <groups><group><name>admins</name><user-name>alice</user-name>
          <user-name>alice</user-name></group></groups>
      </nacm>)xml",
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
        <rule-list><name>rules</name><group>admins</group><group>admins</group>
        </rule-list>
      </nacm>)xml",
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
        <rule-list><name>rules</name><group>*admins</group></rule-list>
      </nacm>)xml",
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm"
                  xmlns:foreign="urn:foreign">
        <foreign:read-default>deny</foreign:read-default>
      </nacm>)xml",
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
        <rule-list><name>rules</name><group>admins</group><rule>
          <name>read</name><rpc-name>get</rpc-name><rpc-name>get-config</rpc-name>
          <action>permit</action></rule></rule-list>
      </nacm>)xml",
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
        <rule-list><name>rules</name><group>admins</group><rule>
          <name>duplicate-bit</name><access-operations>read read</access-operations>
          <action>permit</action></rule></rule-list>
      </nacm>)xml"};
  for (const std::string& xml : invalid) {
    const NacmLoadResult loaded = LoadNacmPolicy(xml);
    EXPECT_FALSE(loaded.policy) << xml;
    EXPECT_FALSE(loaded.errors.empty()) << xml;
  }
}

TEST(NacmTest, RejectsEveryMultipleRuleTypeCombination) {
  const std::vector<std::string> invalid_selectors = {
      "<rpc-name>get</rpc-name>"
      "<notification-name>link-state</notification-name>",
      "<rpc-name>get</rpc-name>"
      "<path xmlns:if=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">"
      "/if:interfaces</path>",
      "<notification-name>link-state</notification-name>"
      "<path xmlns:if=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">"
      "/if:interfaces</path>",
      "<rpc-name>get</rpc-name>"
      "<notification-name>link-state</notification-name>"
      "<path xmlns:if=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">"
      "/if:interfaces</path>"};
  for (size_t index = 0; index < invalid_selectors.size(); ++index) {
    const std::string xml =
        "<nacm xmlns=\"urn:ietf:params:xml:ns:yang:ietf-netconf-acm\">"
        "<rule-list><name>rules</name><group>operators</group><rule>"
        "<name>invalid-" + std::to_string(index) + "</name>" +
        invalid_selectors[index] +
        "<action>deny</action></rule></rule-list></nacm>";
    const NacmLoadResult loaded = LoadNacmPolicy(xml);
    EXPECT_FALSE(loaded.policy) << xml;
    EXPECT_NE(std::find(loaded.errors.begin(), loaded.errors.end(),
                        "NACM rule has multiple rule types"),
              loaded.errors.end()) << xml;
  }
}

TEST(NacmTest, RejectsUnmodeledXmlAttributesAndContainerText) {
  const NacmLoadResult valid = LoadNacmPolicy(R"xml(
    <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm"
          xmlns:unused="urn:unused">
      <groups>
        <group><name>operators</name><user-name>alice</user-name></group>
      </groups>
    </nacm>)xml");
  EXPECT_TRUE(valid.errors.empty());
  EXPECT_TRUE(valid.policy);

  const std::vector<std::string> invalid = {
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm"
                   vendor-option="ignored"/>)xml",
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
        <read-default xml:lang="en">deny</read-default>
      </nacm>)xml",
      R"xml(<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
        <groups>unexpected
          <group><name>operators</name><user-name>alice</user-name></group>
        </groups>
      </nacm>)xml"};
  for (const std::string& xml : invalid) {
    const NacmLoadResult loaded = LoadNacmPolicy(xml);
    EXPECT_FALSE(loaded.policy) << xml;
    EXPECT_FALSE(loaded.errors.empty()) << xml;
  }
}

TEST(NacmTest, DoesNotBroadenExplicitlyEmptyLexicalValues) {
  const NacmLoadResult loaded = LoadNacmPolicy(R"xml(
    <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
      <exec-default>deny</exec-default>
      <groups><group><name>users</name><user-name>alice</user-name>
      </group></groups>
      <rule-list><name>empty-values</name><group>users</group>
        <rule><name>no-operations</name><access-operations></access-operations>
          <action>permit</action></rule>
        <rule><name>empty-rpc</name><rpc-name></rpc-name>
          <access-operations>exec</access-operations><action>permit</action></rule>
        <rule><name>empty-module</name><module-name></module-name>
          <rpc-name>*</rpc-name><access-operations>exec</access-operations>
          <action>permit</action></rule>
      </rule-list>
    </nacm>)xml");
  ASSERT_TRUE(loaded.policy) <<
      (loaded.errors.empty() ? "" : loaded.errors.front());
  EXPECT_FALSE(loaded.policy->AuthorizeRpc("alice", "ietf-netconf", "commit"));
}

TEST(NacmTest, CountsDeniedOperationsWritesAndNotifications) {
  NacmPolicy policy;
  policy.set_exec_default(AccessAction::kDeny);
  policy.set_write_default(AccessAction::kDeny);
  policy.set_read_default(AccessAction::kDeny);
  EXPECT_FALSE(policy.AuthorizeRpc("guest", "example", "reset"));
  EXPECT_FALSE(policy.AuthorizeData("guest", "example",
                                    AccessOperation::kUpdate, "/x"));
  EXPECT_FALSE(policy.AuthorizeNotification("guest", "example", "alarm"));
  const NacmCounters counters = policy.counters();
  EXPECT_EQ(counters.denied_operations, 1U);
  EXPECT_EQ(counters.denied_data_writes, 1U);
  EXPECT_EQ(counters.denied_notifications, 1U);
}

TEST(NacmTest, AppliesOrderedNotificationRulesAndRecoveryBypass) {
  NacmPolicy policy;
  policy.AddUserToGroup("alice", "operators");
  NacmRule deny;
  deny.name = "deny-alarm";
  deny.groups = {"operators"};
  deny.module_name = "example-events";
  deny.notification_name = "alarm";
  deny.operations = AccessMask(AccessOperation::kRead);
  deny.action = AccessAction::kDeny;
  policy.AddRule(std::move(deny));
  policy.AddRecoveryUser("root");
  EXPECT_FALSE(policy.AuthorizeNotification(
      "alice", "example-events", "alarm"));
  EXPECT_TRUE(policy.AuthorizeNotification(
      "alice", "example-events", "link-up"));
  EXPECT_TRUE(policy.AuthorizeNotification(
      "root", "example-events", "alarm"));
}

TEST(NacmTest, MatchesKeyedPathsAndTreatsMissingKeysAsWildcards) {
  const NacmLoadResult loaded = LoadNacmPolicy(R"xml(
    <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm"
          xmlns:if="urn:ietf:params:xml:ns:yang:ietf-interfaces">
      <write-default>deny</write-default>
      <groups><group><name>operators</name><user-name>alice</user-name>
      </group></groups>
      <rule-list><name>interfaces</name><group>operators</group>
        <rule><name>dummy-only</name>
          <path>/if:interfaces/if:interface[if:name='dummy']</path>
          <access-operations>update</access-operations><action>permit</action>
        </rule>
      </rule-list>
    </nacm>)xml");
  ASSERT_TRUE(loaded.errors.empty());
  ASSERT_TRUE(loaded.policy);
  const std::string prefix =
      "/{urn:ietf:params:xml:ns:yang:ietf-interfaces}interfaces/"
      "{urn:ietf:params:xml:ns:yang:ietf-interfaces}interface";
  EXPECT_TRUE(loaded.policy->AuthorizeData(
      "alice", "ietf-interfaces", AccessOperation::kUpdate,
      prefix + "[{urn:ietf:params:xml:ns:yang:ietf-interfaces}name='dummy']/"
               "{urn:ietf:params:xml:ns:yang:ietf-interfaces}enabled"));
  EXPECT_FALSE(loaded.policy->AuthorizeData(
      "alice", "ietf-interfaces", AccessOperation::kUpdate,
      prefix + "[{urn:ietf:params:xml:ns:yang:ietf-interfaces}name='eth0']"));

  NacmPolicy wildcard;
  wildcard.AddUserToGroup("alice", "operators");
  wildcard.AddRule({"all-interfaces", "operators", "", prefix,
                    AccessMask(AccessOperation::kUpdate),
                    AccessAction::kPermit});
  EXPECT_TRUE(wildcard.AuthorizeData(
      "alice", AccessOperation::kUpdate,
      prefix + "[{urn:ietf:params:xml:ns:yang:ietf-interfaces}name='eth0']"));
}

TEST(NacmTest, DoesNotUseTextualPathPrefixes) {
  NacmPolicy policy;
  policy.AddUserToGroup("alice", "operators");
  policy.AddRule({"interfaces", "operators", "", "/{urn:example}interface",
                  AccessMask(AccessOperation::kUpdate),
                  AccessAction::kPermit});
  EXPECT_FALSE(policy.AuthorizeData(
      "alice", AccessOperation::kUpdate, "/{urn:example}interfaces"));
}

TEST(NacmTest, FiltersSpecificKeyedListInstances) {
  const NacmLoadResult loaded = LoadNacmPolicy(R"xml(
    <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm"
          xmlns:if="urn:ietf:params:xml:ns:yang:ietf-interfaces">
      <groups><group><name>guests</name><user-name>alice</user-name></group></groups>
      <rule-list><name>hide</name><group>guests</group>
        <rule><name>hide-private</name>
          <path>/if:interfaces/if:interface[if:name='private']</path>
          <access-operations>read</access-operations><action>deny</action>
        </rule>
      </rule-list>
    </nacm>)xml");
  ASSERT_TRUE(loaded.policy);
  const std::string filtered = loaded.policy->FilterReadableData("alice", R"xml(
    <data xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <interfaces xmlns="urn:ietf:params:xml:ns:yang:ietf-interfaces">
        <interface><name>public</name><enabled>true</enabled></interface>
        <interface><name>private</name><enabled>false</enabled></interface>
      </interfaces>
    </data>)xml");
  EXPECT_NE(filtered.find("<name>public</name>"), std::string::npos);
  EXPECT_EQ(filtered.find("<name>private</name>"), std::string::npos);
}

TEST(NacmTest, EnforcesDefaultDenyAnnotationsAfterExplicitRules) {
  NacmPolicy policy;
  policy.set_read_default(AccessAction::kPermit);
  policy.set_write_default(AccessAction::kPermit);
  policy.set_exec_default(AccessAction::kPermit);
  EXPECT_FALSE(policy.AuthorizeData("alice", "secure",
      AccessOperation::kRead, "/{urn:secure}secret", {}, true, false));
  EXPECT_FALSE(policy.AuthorizeData("alice", "secure",
      AccessOperation::kUpdate, "/{urn:secure}setting", {}, false, true));
  EXPECT_TRUE(policy.AuthorizeData("alice", "secure",
      AccessOperation::kRead, "/{urn:secure}setting", {}, false, true));
  EXPECT_FALSE(policy.AuthorizeRpc("alice", "secure", "reset", {}, true));
  EXPECT_FALSE(policy.AuthorizeNotification(
      "alice", "secure", "alarm", {}, true));

  policy.AddUserToGroup("alice", "operators");
  policy.AddRule({"permit-secret", "operators", "", "/{urn:secure}secret",
                  AccessMask(AccessOperation::kRead),
                  AccessAction::kPermit});
  EXPECT_TRUE(policy.AuthorizeData("alice", "secure",
      AccessOperation::kRead, "/{urn:secure}secret", {}, true, false));
}

TEST(NacmTest, AppliesRfc8341ActionDecisionSequence) {
  NacmPolicy policy;
  policy.AddUserToGroup("alice", "operators");
  policy.AddRule({"read-system", "operators", "", "/{urn:secure}system",
                  AccessMask(AccessOperation::kRead),
                  AccessAction::kPermit});
  policy.AddRule({"reset-system", "operators", "",
                  "/{urn:secure}system/{urn:secure}reset",
                  AccessMask(AccessOperation::kExecute),
                  AccessAction::kPermit});
  const std::vector<NacmDataNode> ancestors = {
      {"secure", "/{urn:secure}system", false}};
  EXPECT_TRUE(policy.AuthorizeAction(
      "alice", "secure", "reset",
      "/{urn:secure}system/{urn:secure}reset", ancestors));

  NacmPolicy hidden_parent;
  hidden_parent.AddUserToGroup("alice", "operators");
  hidden_parent.AddRule({"reset-system", "operators", "",
                         "/{urn:secure}system/{urn:secure}reset",
                         AccessMask(AccessOperation::kExecute),
                         AccessAction::kPermit});
  hidden_parent.set_read_default(AccessAction::kDeny);
  EXPECT_FALSE(hidden_parent.AuthorizeAction(
      "alice", "secure", "reset",
      "/{urn:secure}system/{urn:secure}reset", ancestors));
  EXPECT_EQ(hidden_parent.counters().denied_operations, 1U);
}

TEST(NacmTest, AppliesRfc8341DataAssociatedNotificationDecisionSequence) {
  NacmPolicy policy;
  policy.AddUserToGroup("alice", "operators");
  policy.AddRule({"read-interface", "operators", "",
                  "/{urn:example}interfaces/{urn:example}interface",
                  AccessMask(AccessOperation::kRead),
                  AccessAction::kPermit});
  NacmRule event;
  event.name = "read-link-event";
  event.groups = {"operators"};
  event.module_name = "example";
  event.notification_name = "link-change";
  event.operations = AccessMask(AccessOperation::kRead);
  event.action = AccessAction::kPermit;
  policy.AddRule(std::move(event));
  const std::vector<NacmDataNode> ancestors = {
      {"example", "/{urn:example}interfaces", false},
      {"example", "/{urn:example}interfaces/{urn:example}interface", false}};
  EXPECT_TRUE(policy.AuthorizeNotification(
      "alice", "example", "link-change",
      "/{urn:example}interfaces/{urn:example}interface/"
      "{urn:example}link-change", ancestors));

  policy.set_read_default(AccessAction::kDeny);
  EXPECT_FALSE(policy.AuthorizeNotification(
      "bob", "example", "link-change",
      "/{urn:example}interfaces/{urn:example}interface/"
      "{urn:example}link-change", ancestors));
  EXPECT_EQ(policy.counters().denied_notifications, 1U);
}

TEST(NacmTest, PreservesRecoveryIdentityWhenManagedPolicyChanges) {
  NacmPolicy previous;
  previous.AddRecoveryUser("break-glass");
  previous.set_exec_default(AccessAction::kDeny);
  EXPECT_FALSE(previous.AuthorizeRpc("guest", "delete-config"));

  NacmPolicy replacement;
  replacement.set_exec_default(AccessAction::kDeny);
  replacement.PreserveRuntimeStateFrom(previous);
  EXPECT_TRUE(replacement.AuthorizeRpc("break-glass", "delete-config"));
  EXPECT_EQ(replacement.counters().denied_operations, 1U);
}

}  // namespace
}  // namespace yang::netconf
