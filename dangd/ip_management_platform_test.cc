// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugins/ip_management/platform_config.h"

#include <gtest/gtest.h>

namespace dangd::ip_management {
namespace {

TEST(IpManagementPlatformTest, ExtractsNamespacedIpv4AndIpv6Intent) {
  constexpr std::string_view xml = R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <if:interfaces xmlns:if="urn:ietf:params:xml:ns:yang:ietf-interfaces"
                     xmlns:ip="urn:ietf:params:xml:ns:yang:ietf-ip">
        <if:interface><if:name>em0</if:name><if:enabled>true</if:enabled>
          <ip:ipv4><ip:address><ip:ip>192.0.2.4</ip:ip>
            <ip:prefix-length>24</ip:prefix-length></ip:address></ip:ipv4>
          <ip:ipv6><ip:address><ip:ip>2001:db8::4</ip:ip>
            <ip:prefix-length>64</ip:prefix-length></ip:address></ip:ipv6>
        </if:interface>
      </if:interfaces>
    </config>)xml";
  std::vector<InterfaceConfig> interfaces;
  std::string error;
  ASSERT_TRUE(ParsePlatformConfig(xml, &interfaces, &error)) << error;
  ASSERT_EQ(interfaces.size(), 1u);
  EXPECT_EQ(interfaces[0].name, "em0");
  EXPECT_EQ(interfaces[0].enabled, true);
  EXPECT_EQ(interfaces[0].addresses,
            (std::vector<AddressConfig>{{"192.0.2.4", 24, false},
                                        {"2001:db8::4", 64, true}}));
}

TEST(IpManagementPlatformTest, RejectsMalformedConfiguration) {
  std::vector<InterfaceConfig> interfaces;
  std::string error;
  EXPECT_FALSE(ParsePlatformConfig("<config>", &interfaces, &error));
  EXPECT_NE(error.find("cannot parse configuration"), std::string::npos);
}

TEST(IpManagementPlatformTest, RejectsOptionLikeInterfaceName) {
  constexpr std::string_view xml =
      "<interfaces><interface><name>--help</name></interface></interfaces>";
  std::vector<InterfaceConfig> interfaces;
  std::string error;
  EXPECT_FALSE(ParsePlatformConfig(xml, &interfaces, &error));
  EXPECT_NE(error.find("begin with '-'"), std::string::npos);
}

}  // namespace
}  // namespace dangd::ip_management
