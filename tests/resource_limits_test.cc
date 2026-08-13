// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <gtest/gtest.h>
#include <pugixml.hpp>

#include "yang/parser.h"
#include "yang/resource_limits.h"
#include "yang/source_file.h"

namespace yang {
namespace {

TEST(ResourceLimitsTest, EnforcesSourceBytesAtExactBoundary) {
  ResourceLimits limits;
  limits.maximum_source_bytes = 4;
  VectorDiagnosticSink accepted_diagnostics;
  EXPECT_TRUE(SourceFile::Create("accepted.yang", "leaf",
                                 accepted_diagnostics, limits));
  VectorDiagnosticSink rejected_diagnostics;
  EXPECT_FALSE(SourceFile::Create("rejected.yang", "leafx",
                                  rejected_diagnostics, limits));
  ASSERT_FALSE(rejected_diagnostics.diagnostics().empty());
  EXPECT_EQ(rejected_diagnostics.diagnostics().back().code,
            DiagnosticCode::kResourceLimitExceeded);
}

TEST(ResourceLimitsTest, EnforcesYangDepthAndStatementCount) {
  ResourceLimits depth_limits;
  depth_limits.maximum_yang_depth = 2;
  VectorDiagnosticSink depth_diagnostics;
  auto depth_source = SourceFile::Create(
      "depth.yang", "container a { container b { leaf c; } }",
      depth_diagnostics, depth_limits);
  ASSERT_TRUE(depth_source);
  Parser depth_parser(depth_source, depth_diagnostics, depth_limits);
  (void)depth_parser.Parse();
  EXPECT_TRUE(depth_diagnostics.has_errors());

  ResourceLimits count_limits;
  count_limits.maximum_statements = 2;
  VectorDiagnosticSink count_diagnostics;
  auto count_source = SourceFile::Create(
      "count.yang", "leaf a; leaf b; leaf c;", count_diagnostics,
      count_limits);
  ASSERT_TRUE(count_source);
  Parser count_parser(count_source, count_diagnostics, count_limits);
  (void)count_parser.Parse();
  EXPECT_TRUE(count_diagnostics.has_errors());
  EXPECT_EQ(count_diagnostics.diagnostics().back().code,
            DiagnosticCode::kResourceLimitExceeded);
}

TEST(ResourceLimitsTest, EnforcesXmlNodesAndDepthAtBoundaries) {
  pugi::xml_document document;
  ASSERT_TRUE(document.load_string("<a><b/><c/></a>"));
  ResourceLimits limits;
  limits.maximum_xml_bytes = 18;
  limits.maximum_xml_nodes = 3;
  limits.maximum_xml_depth = 2;
  std::string error;
  EXPECT_TRUE(XmlWithinResourceLimits(document, "<a><b/><c/></a>", limits,
                                      &error));
  limits.maximum_xml_nodes = 2;
  EXPECT_FALSE(XmlWithinResourceLimits(document, "<a><b/><c/></a>", limits,
                                       &error));
  limits.maximum_xml_nodes = 3;
  limits.maximum_xml_depth = 1;
  EXPECT_FALSE(XmlWithinResourceLimits(document, "<a><b/><c/></a>", limits,
                                       &error));
}

TEST(ResourceLimitsTest, BoundsXPathWithoutCountingQuotedSlashes) {
  ResourceLimits limits;
  limits.maximum_xpath_bytes = 64;
  limits.maximum_xpath_steps = 3;
  std::string error;
  EXPECT_TRUE(XPathWithinResourceLimits("/a/b[.='x/y']", limits, &error));
  EXPECT_FALSE(XPathWithinResourceLimits("/a/b/c", limits, &error));
  limits.maximum_xpath_steps = 4;
  EXPECT_TRUE(XPathWithinResourceLimits("/a/b/c", limits, &error));
}

}  // namespace
}  // namespace yang
