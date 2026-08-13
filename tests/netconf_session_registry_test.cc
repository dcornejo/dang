// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/netconf_session_registry.h"

namespace yang::netconf {
namespace {

TEST(NetconfSessionRegistryTest, EnforcesUniqueNonzeroSessionIds) {
  SessionRegistry registry;
  EXPECT_FALSE(registry.Register(0, "alice"));
  EXPECT_FALSE(registry.Register(1, ""));
  ASSERT_TRUE(registry.Register(1, "alice"));
  EXPECT_FALSE(registry.Register(1, "bob"));
  const auto session = registry.Find(1);
  ASSERT_TRUE(session);
  EXPECT_EQ(session->username, "alice");
  ASSERT_EQ(registry.List().size(), 1U);
}

TEST(NetconfSessionRegistryTest, SignalsOnlyActiveTargets) {
  SessionRegistry registry;
  ASSERT_TRUE(registry.Register(7, "alice"));
  EXPECT_FALSE(registry.CloseRequested(7));
  EXPECT_TRUE(registry.RequestClose(7));
  EXPECT_TRUE(registry.CloseRequested(7));
  EXPECT_FALSE(registry.RequestClose(8));
  EXPECT_TRUE(registry.Unregister(7));
  EXPECT_FALSE(registry.CloseRequested(7));
}

}  // namespace
}  // namespace yang::netconf
