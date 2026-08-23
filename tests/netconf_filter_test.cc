// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/netconf_filter.h"

namespace yang::netconf {
namespace {

constexpr std::string_view kData = R"xml(
  <data xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
    <system xmlns="urn:example">
      <hostname>router</hostname>
      <interface><name>eth0</name><enabled>true</enabled></interface>
      <interface><name>eth1</name><enabled>false</enabled></interface>
    </system>
    <state xmlns="urn:other"><uptime>10</uptime></state>
  </data>)xml";

TEST(NetconfFilterTest, SelectsAnEntireSubtree) {
  FilterResult result = ApplySubtreeFilter(kData, R"xml(
    <filter xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:example"><hostname/></system>
    </filter>)xml");
  ASSERT_TRUE(result.xml) << result.error.value_or("");
  EXPECT_NE(result.xml->find("<hostname>router</hostname>"), std::string::npos);
  EXPECT_EQ(result.xml->find("<interface>"), std::string::npos);
  EXPECT_EQ(result.xml->find("<state"), std::string::npos);
}

TEST(NetconfFilterTest, AppliesSiblingContentMatchNodes) {
  FilterResult result = ApplySubtreeFilter(kData, R"xml(
    <filter xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:example"><interface>
        <name>eth1</name><enabled/>
      </interface></system>
    </filter>)xml");
  ASSERT_TRUE(result.xml) << result.error.value_or("");
  EXPECT_NE(result.xml->find("<name>eth1</name>"), std::string::npos);
  EXPECT_NE(result.xml->find("<enabled>false</enabled>"), std::string::npos);
  EXPECT_EQ(result.xml->find("eth0"), std::string::npos);
}

TEST(NetconfFilterTest, SupportsNamespaceWildcard) {
  FilterResult wildcard = ApplySubtreeFilter(kData, R"xml(
    <filter xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <state xmlns=""/>
    </filter>)xml");
  ASSERT_TRUE(wildcard.xml);
  EXPECT_NE(wildcard.xml->find("<uptime>10</uptime>"), std::string::npos);
}

TEST(NetconfFilterTest, AppliesXPathAndIncludesAncestorPath) {
  FilterResult result = ApplyXPathFilter(kData, R"xml(
    <filter xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
            xmlns:e="urn:example" type="xpath"
            select="/e:system/e:interface[e:name='eth1']"/>)xml");
  ASSERT_TRUE(result.xml) << result.error.value_or("");
  EXPECT_NE(result.xml->find("e:system"), std::string::npos);
  EXPECT_NE(result.xml->find("eth1"), std::string::npos);
  EXPECT_NE(result.xml->find("false"), std::string::npos);
  EXPECT_EQ(result.xml->find("eth0"), std::string::npos);
  EXPECT_EQ(result.xml->find("uptime"), std::string::npos);
}

TEST(NetconfFilterTest, RejectsInvalidXPathFilterForms) {
  FilterResult scalar = ApplyXPathFilter(
      kData, "<filter type=\"xpath\" select=\"count(/*)\"/>");
  EXPECT_FALSE(scalar.xml);
  EXPECT_EQ(scalar.error_tag, "invalid-value");
  FilterResult missing = ApplyXPathFilter(kData, "<filter type=\"xpath\"/>");
  EXPECT_EQ(missing.error_tag, "missing-attribute");
  FilterResult content = ApplyXPathFilter(
      kData, "<filter type=\"xpath\" select=\"/*\"><x/></filter>");
  EXPECT_EQ(content.error_tag, "invalid-value");
  FilterResult malformed = ApplyXPathFilter(
      kData, "<filter type=\"xpath\" select=\"/[\"/>");
  EXPECT_EQ(malformed.error_tag, "invalid-value");
}

TEST(NetconfFilterTest, AppliesMaximumDepthFromEachXPathSelection) {
  FilterResult result = ApplyXPathFilter(kData, R"xml(
    <filter xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
            xmlns:e="urn:example" type="xpath"
            select="/e:system/e:interface[e:name='eth1']"/>)xml", 1);
  ASSERT_TRUE(result.xml) << result.error.value_or("");
  EXPECT_NE(result.xml->find("e:system"), std::string::npos);
  EXPECT_NE(result.xml->find("e:interface"), std::string::npos);
  EXPECT_EQ(result.xml->find("eth1"), std::string::npos) << *result.xml;

  result = ApplyXPathFilter(kData, R"xml(
    <filter xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
            xmlns:e="urn:example" type="xpath"
            select="/e:system/e:interface[e:name='eth1']"/>)xml", 2);
  ASSERT_TRUE(result.xml) << result.error.value_or("");
  EXPECT_NE(result.xml->find("eth1"), std::string::npos) << *result.xml;
  EXPECT_NE(result.xml->find("false"), std::string::npos) << *result.xml;
  EXPECT_EQ(result.xml->find("eth0"), std::string::npos) << *result.xml;
}

}  // namespace
}  // namespace yang::netconf
