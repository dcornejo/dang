// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "yang/syntax_tree.h"
#include "test_support.h"
namespace yang { namespace {
TEST(SyntaxTreeTest, IdsAndAddressesRemainStable) {
  VectorDiagnosticSink sink; SyntaxTree tree(test::Source("", sink));
  const auto first = tree.Add(Statement{.keyword = "module"}); Statement* address = &tree.Get(first);
  for (int i = 0; i < 10000; ++i) { const auto ignored = tree.Add(Statement{.keyword = "leaf"}); (void)ignored; }
  EXPECT_EQ(first, 0); EXPECT_EQ(&tree.Get(first), address); EXPECT_EQ(tree.Get(first).keyword, "module");
}
} }  // namespace yang
