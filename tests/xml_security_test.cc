// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/xml_security.h"

#include <gtest/gtest.h>

namespace yang {
namespace {

TEST(XmlSecurityTest, RejectsEmbeddedNulMalformedUtf8AndForbiddenCharacters) {
  pugi::xml_document document;
  std::string nul = "<root>safe";
  nul.push_back('\0');
  nul += "hidden</root>";
  EXPECT_FALSE(ParseUntrustedXml(nul, &document).ok);
  const std::string malformed = "<root>\xc0\xaf</root>";
  EXPECT_FALSE(ParseUntrustedXml(malformed, &document).ok);
  const std::string control = "<root>\x01</root>";
  EXPECT_FALSE(ParseUntrustedXml(control, &document).ok);
}

TEST(XmlSecurityTest, RejectsDtdEntitiesAndMultipleRoots) {
  pugi::xml_document document;
  EXPECT_FALSE(ParseUntrustedXml(
      "<!DOCTYPE root [<!ENTITY x 'expanded'>]><root>&x;</root>",
      &document).ok);
  EXPECT_FALSE(ParseUntrustedXml("<first/><second/>", &document).ok);
}

TEST(XmlSecurityTest, AcceptsInertCommentsCdataProcessingInstructionsAndXinclude) {
  pugi::xml_document document;
  const auto parsed = ParseUntrustedXml(
      "<?audit inert?><root><!--note--><![CDATA[<safe>]]>"
      "<xi:include xmlns:xi='http://www.w3.org/2001/XInclude' href='x'/>"
      "</root>",
      &document);
  ASSERT_TRUE(parsed.ok) << parsed.message;
  EXPECT_STREQ(document.document_element().name(), "root");
  EXPECT_STREQ(document.document_element().text().as_string(), "<safe>");
  EXPECT_TRUE(document.document_element().child("xi:include"));
}

TEST(XmlSecurityTest, AllowsExplicitFragmentPolicy) {
  pugi::xml_document document;
  UntrustedXmlPolicy policy;
  policy.require_single_document_element = false;
  EXPECT_TRUE(ParseUntrustedXml("<first/><second/>", &document, policy).ok);
}

TEST(XmlSecurityTest, EscapesMarkupAndRepairsInvalidOutputCharacters) {
  std::string value = "<&>\"'";
  value.push_back('\0');
  value += "\xc0\xaf";
  value += " caf\xc3\xa9";
  const std::string escaped = EscapeXmlText(value);
  EXPECT_EQ(escaped,
            "&lt;&amp;&gt;&quot;&apos;\xef\xbf\xbd"
            "\xef\xbf\xbd\xef\xbf\xbd caf\xc3\xa9");
  pugi::xml_document document;
  EXPECT_TRUE(ParseUntrustedXml("<root>" + escaped + "</root>", &document).ok);
}

}  // namespace
}  // namespace yang
