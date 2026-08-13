// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "yang/schema_registry.h"
namespace yang { namespace {
TEST(SchemaRegistryTest, ContainsCoreAndYinMetadata) {
  auto description = SchemaRegistry::Find("description"); ASSERT_TRUE(description);
  EXPECT_EQ(description->yin_argument, "text"); EXPECT_TRUE(description->yin_element);
  EXPECT_FALSE(SchemaRegistry::Find("vendor:thing")); EXPECT_EQ(SchemaRegistry::Builtins().size(), 68);
  EXPECT_FALSE(SchemaRegistry::IsAvailable("action", YangVersion::k1));
  EXPECT_TRUE(SchemaRegistry::IsAvailable("action", YangVersion::k1_1));
  EXPECT_TRUE(SchemaRegistry::IsAllowed("container", "leaf"));
  EXPECT_FALSE(SchemaRegistry::IsAllowed("leaf", "container"));
  EXPECT_TRUE(SchemaRegistry::IsRepeatable("leaf-list", "default"));
  EXPECT_EQ(SchemaRegistry::StatementOrder("revision"), 4);
  ASSERT_EQ(SchemaRegistry::RequiredChildren("module").size(), 2U);
}
} }  // namespace yang
