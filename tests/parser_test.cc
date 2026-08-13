// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "yang/parser.h"
#include "test_support.h"
namespace yang { namespace {
TEST(ParserTest, ParsesNestedModuleAndConcatenatedUtf8) {
  VectorDiagnosticSink sink; auto source = test::Source(R"yang(module sensors {
    yang-version 1.1;
    description "温度 " + 'sensor';
    container readings { leaf value { type string; } }
  })yang", sink);
  Parser parser(source, sink); SyntaxTree tree = parser.Parse();
  ASSERT_FALSE(sink.has_errors()); ASSERT_EQ(tree.roots().size(), 1);
  const Statement& module = tree.Get(tree.roots()[0]); EXPECT_EQ(module.keyword, "module"); EXPECT_EQ(module.argument, "sensors");
  ASSERT_EQ(module.children.size(), 3); EXPECT_EQ(tree.Get(module.children[1]).argument, "温度 sensor");
  EXPECT_EQ(tree.Get(module.children[2]).children.size(), 1);
}
TEST(ParserTest, AcceptsExtensionKeyword) {
  VectorDiagnosticSink sink; Parser parser(test::Source("acme:widget value;", sink), sink); auto tree = parser.Parse();
  ASSERT_EQ(tree.roots().size(), 1); EXPECT_EQ(tree.Get(tree.roots()[0]).keyword, "acme:widget");
}
TEST(ParserTest, RecoversAndParsesFollowingStatement) {
  VectorDiagnosticSink sink; Parser parser(test::Source("@bad x; leaf good;", sink), sink); auto tree = parser.Parse();
  ASSERT_TRUE(sink.has_errors()); ASSERT_EQ(tree.roots().size(), 1); EXPECT_EQ(tree.Get(tree.roots()[0]).keyword, "leaf");
}
TEST(ParserTest, ReportsMissingClosingBrace) {
  VectorDiagnosticSink sink; Parser parser(test::Source("module x { leaf y;", sink), sink); const auto tree = parser.Parse(); (void)tree;
  ASSERT_TRUE(sink.has_errors()); EXPECT_EQ(sink.diagnostics().back().code, DiagnosticCode::kMissingRightBrace);
}
} }  // namespace yang
