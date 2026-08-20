// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/parser.h"
#include "yang/validator.h"
#include "test_support.h"

namespace yang {
namespace {

SyntaxTree Parse(std::string text, VectorDiagnosticSink& sink) {
  Parser parser(test::Source(std::move(text), sink), sink);
  return parser.Parse();
}

TEST(ValidatorTest, AcceptsStructurallyValidModule) {
  VectorDiagnosticSink sink;
  const SyntaxTree tree = Parse(R"yang(module example {
    yang-version 1.1;
    namespace "urn:example";
    prefix ex;
    revision 2026-08-12 { description "Initial revision."; }
    container system {
      leaf hostname { type string; mandatory true; }
    }
  })yang", sink);
  Validator validator(sink);
  EXPECT_TRUE(validator.Validate(tree));
  EXPECT_FALSE(sink.has_errors());
}

TEST(ValidatorTest, RequiresModuleHeaderAndLeafType) {
  VectorDiagnosticSink sink;
  const SyntaxTree tree = Parse("module broken { leaf name; }", sink);
  Validator validator(sink);
  EXPECT_FALSE(validator.Validate(tree));
  ASSERT_EQ(sink.diagnostics().size(), 3);
  EXPECT_EQ(sink.diagnostics()[0].code, DiagnosticCode::kMissingRequiredStatement);
  EXPECT_EQ(sink.diagnostics()[2].code, DiagnosticCode::kMissingRequiredStatement);
}

TEST(ValidatorTest, RejectsDuplicateSingletonAndBadChild) {
  VectorDiagnosticSink sink;
  const SyntaxTree tree = Parse(R"yang(module bad {
    namespace "urn:bad";
    namespace "urn:again";
    prefix b;
    leaf x { type string; prefix nope; }
  })yang", sink);
  Validator validator(sink);
  EXPECT_FALSE(validator.Validate(tree));
  bool duplicate = false;
  bool invalid_child = false;
  for (const auto& diagnostic : sink.diagnostics()) {
    duplicate |= diagnostic.code == DiagnosticCode::kDuplicateStatement;
    invalid_child |= diagnostic.code == DiagnosticCode::kInvalidSubstatement;
  }
  EXPECT_TRUE(duplicate);
  EXPECT_TRUE(invalid_child);
}

TEST(ValidatorTest, EnforcesModuleSectionOrder) {
  VectorDiagnosticSink sink;
  const SyntaxTree tree = Parse(R"yang(module bad {
    namespace "urn:bad";
    prefix b;
    description "late header";
    import other { prefix o; }
  })yang", sink);
  Validator validator(sink);
  EXPECT_FALSE(validator.Validate(tree));
  EXPECT_EQ(sink.diagnostics().back().code, DiagnosticCode::kInvalidStatementOrder);
}

TEST(ValidatorTest, EnforcesYangVersionGates) {
  VectorDiagnosticSink sink;
  auto source = test::Source(R"yang(module old {
    namespace "urn:old"; prefix old;
    container root { action reset; }
  })yang", sink);
  Parser parser(source, sink);
  const SyntaxTree tree = parser.Parse();
  Validator validator(sink);
  EXPECT_FALSE(validator.Validate(tree));
  EXPECT_EQ(sink.diagnostics().back().code,
            DiagnosticCode::kStatementRequiresYang11);

  VectorDiagnosticSink modern_sink;
  auto modern_source = test::Source(R"yang(module modern {
    yang-version 1.1;
    namespace "urn:modern"; prefix modern;
    container root { action reset; }
  })yang", modern_sink);
  Parser modern_parser(modern_source, modern_sink);
  const SyntaxTree modern_tree = modern_parser.Parse();
  Validator modern_validator(modern_sink);
  EXPECT_TRUE(modern_validator.Validate(modern_tree));
}

TEST(ValidatorTest, RejectsUnsupportedYangVersion) {
  VectorDiagnosticSink sink;
  auto source = test::Source(R"yang(module future {
    yang-version 2;
    namespace "urn:future"; prefix future;
  })yang", sink);
  Parser parser(source, sink);
  const SyntaxTree tree = parser.Parse();
  Validator validator(sink);
  EXPECT_FALSE(validator.Validate(tree));
  EXPECT_EQ(sink.diagnostics().front().code,
            DiagnosticCode::kUnsupportedYangVersion);
}

TEST(ValidatorTest, AcceptsStandardStatementsInsideExtensionInvocation) {
  VectorDiagnosticSink sink;
  const SyntaxTree tree = Parse(R"yang(module extension-test {
    yang-version 1.1;
    namespace "urn:extension-test";
    prefix et;
    et:annotation origin {
      type string;
      description "Extension-defined content.";
    }
  })yang", sink);
  Validator validator(sink);
  EXPECT_TRUE(validator.Validate(tree));
  EXPECT_FALSE(sink.has_errors());
}

}  // namespace
}  // namespace yang
