// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "yang/lexer.h"
#include "yang/string_decoder.h"
#include "test_support.h"
namespace yang { namespace {
std::optional<std::string> Decode(std::string text, VectorDiagnosticSink& sink) {
  Lexer lexer(test::Source(std::move(text), sink), sink); StringDecoder decoder(sink); return decoder.Decode(lexer.Next());
}
TEST(StringDecoderTest, PreservesSingleQuotedUtf8) {
  VectorDiagnosticSink sink; EXPECT_EQ(Decode("'温度\\n'", sink), "温度\\n");
}
TEST(StringDecoderTest, ProcessesAllowedDoubleQuotedEscapes) {
  VectorDiagnosticSink sink; EXPECT_EQ(Decode(R"("a\n\t\"\\b")", sink), "a\n\t\"\\b");
}
TEST(StringDecoderTest, RejectsUnknownEscape) {
  VectorDiagnosticSink sink; EXPECT_FALSE(Decode(R"("\q")", sink)); EXPECT_TRUE(sink.has_errors());
}
TEST(StringDecoderTest, TrimsMultilineIndentationAndTrailingSpace) {
  VectorDiagnosticSink sink; EXPECT_EQ(Decode("  \"first  \n   second\"", sink), "first\nsecond");
}
} }  // namespace yang
