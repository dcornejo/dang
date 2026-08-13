// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "yang/lexer.h"
#include "test_support.h"
namespace yang { namespace {
TEST(LexerTest, LexesStatementsAndSkipsComments) {
  VectorDiagnosticSink sink; Lexer lexer(test::Source("module x { // hi\n leaf y; /* ok */ }", sink), sink);
  EXPECT_EQ(lexer.Next().text(), "module"); EXPECT_EQ(lexer.Next().text(), "x");
  EXPECT_EQ(lexer.Next().kind, TokenKind::kLeftBrace); EXPECT_EQ(lexer.Next().text(), "leaf");
  EXPECT_EQ(lexer.Next().text(), "y"); EXPECT_EQ(lexer.Next().kind, TokenKind::kSemicolon);
  EXPECT_EQ(lexer.Next().kind, TokenKind::kRightBrace); EXPECT_EQ(lexer.Next().kind, TokenKind::kEndOfFile);
  EXPECT_FALSE(sink.has_errors());
}
TEST(LexerTest, CommentDelimiterTerminatesUnquotedText) {
  VectorDiagnosticSink sink; Lexer lexer(test::Source("hello/*x*/world", sink), sink);
  EXPECT_EQ(lexer.Next().text(), "hello"); EXPECT_EQ(lexer.Next().text(), "world");
}
TEST(LexerTest, TracksUnicodeColumns) {
  VectorDiagnosticSink sink; Lexer lexer(test::Source("'日本' next", sink), sink);
  const Token ignored = lexer.Next(); (void)ignored; EXPECT_EQ(lexer.Next().range.begin.column, 6);
}
TEST(LexerTest, ReportsUnterminatedConstructs) {
  VectorDiagnosticSink sink; Lexer lexer(test::Source("/* no", sink), sink);
  const Token ignored = lexer.Next(); (void)ignored; ASSERT_EQ(sink.diagnostics().size(), 1);
  EXPECT_EQ(sink.diagnostics()[0].code, DiagnosticCode::kUnterminatedBlockComment);
}
TEST(LexerTest, RejectsStandaloneCarriageReturnOutsideQuotedString) {
  VectorDiagnosticSink sink;
  Lexer lexer(test::Source("module\rx; 'quoted\rvalue';", sink), sink);
  while (lexer.Next().kind != TokenKind::kEndOfFile) {}
  ASSERT_EQ(sink.diagnostics().size(), 1U);
  EXPECT_EQ(sink.diagnostics()[0].code, DiagnosticCode::kInvalidLineEnding);
}
} }  // namespace yang
