// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "yang/source_file.h"
namespace yang { namespace {
TEST(SourceFileTest, OwnsValidUtf8) {
  VectorDiagnosticSink sink; auto source = SourceFile::Create("x.yang", "日本語", sink);
  ASSERT_NE(source, nullptr); EXPECT_EQ(source->contents(), "日本語"); EXPECT_FALSE(sink.has_errors());
}
TEST(SourceFileTest, RejectsInvalidUtf8AtPreciseLocation) {
  VectorDiagnosticSink sink; auto source = SourceFile::Create("x", std::string("ok\xC0", 3), sink);
  EXPECT_EQ(source, nullptr); ASSERT_EQ(sink.diagnostics().size(), 1); EXPECT_EQ(sink.diagnostics()[0].range.begin.column, 3);
}
TEST(SourceFileTest, CountsUnicodeCodePointColumns) {
  VectorDiagnosticSink sink; std::string input = "日本"; input.push_back('\a');
  EXPECT_EQ(SourceFile::Create("x", input, sink), nullptr);
  ASSERT_EQ(sink.diagnostics().size(), 1); EXPECT_EQ(sink.diagnostics()[0].range.begin.column, 3);
}
TEST(SourceFileTest, FormatsSourceExcerptAndRelatedLocation) {
  VectorDiagnosticSink sink;
  auto source = SourceFile::Create("example.yang", "leaf value;\n", sink);
  ASSERT_TRUE(source);
  Diagnostic diagnostic{DiagnosticCode::kUnexpectedToken,
                        DiagnosticSeverity::kError, "expected type",
                        {{5, 1, 6}, {10, 1, 11}}};
  diagnostic.related.push_back({"declared here", {{0, 1, 1}, {4, 1, 5}},
                                "example.yang"});
  const std::string formatted = FormatDiagnostic(diagnostic, source.get());
  EXPECT_NE(formatted.find("example.yang:1:6: error kUnexpectedToken"),
            std::string::npos);
  EXPECT_NE(formatted.find("     ^^^^^"), std::string::npos);
  EXPECT_NE(formatted.find("note: example.yang:1:1: declared here"),
            std::string::npos);
}
} }  // namespace yang
