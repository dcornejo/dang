// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "yang/utf8.h"
namespace yang::utf8 { namespace {
TEST(Utf8Test, DecodesEverySequenceLength) {
  EXPECT_EQ(Decode("A")->code_point, U'A');
  EXPECT_EQ(Decode("é")->code_point, U'é');
  EXPECT_EQ(Decode("日")->code_point, U'日');
  EXPECT_EQ(Decode("😀")->code_point, U'😀');
}
TEST(Utf8Test, RejectsMalformedSequences) {
  EXPECT_FALSE(Decode(std::string_view("\xC0\xAF", 2)));
  EXPECT_FALSE(Decode(std::string_view("\xED\xA0\x80", 3)));
  EXPECT_FALSE(Decode(std::string_view("\xF4\x90\x80\x80", 4)));
  EXPECT_FALSE(Decode(std::string_view("\xE2\x82", 2)));
}
TEST(Utf8Test, AppliesYangCharacterRules) {
  EXPECT_TRUE(IsYangCharacter(U'\t')); EXPECT_TRUE(IsYangCharacter(U'\n'));
  EXPECT_FALSE(IsYangCharacter(U'\a')); EXPECT_FALSE(IsYangCharacter(0xffff));
}
} }  // namespace yang::utf8
