// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_manager.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace dangd {
namespace {

TEST(PluginManagerTest, FailedDiscoveryDoesNotPublishPartialManifest) {
  PluginManager plugins;
  std::vector<std::string> errors;
  EXPECT_FALSE(plugins.Load(DANG_TEST_BROKEN_PLUGIN_PATH, &errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.front().find("simulated discovery failure"),
            std::string::npos);
  EXPECT_TRUE(plugins.yang_sources().empty());
  EXPECT_TRUE(plugins.manifests().empty());
}

TEST(PluginManagerTest, RejectsMissingRuntimeDependency) {
  PluginManager plugins;
  std::vector<std::string> errors;
  ASSERT_TRUE(plugins.Load(DANG_TEST_CONSUMER_PLUGIN_PATH, &errors));
  EXPECT_FALSE(plugins.ValidateDependencies(&errors));
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(errors.back().find("requires missing implementation module"),
            std::string::npos);
}

}  // namespace
}  // namespace dangd
