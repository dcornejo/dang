// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_TEST_PLUGINS_PLUGIN_TEST_SUPPORT_H_
#define DANGD_TEST_PLUGINS_PLUGIN_TEST_SUPPORT_H_

#include <string>
#include <string_view>
#include <vector>

namespace dangd::test_plugin {

/** Clears the shared callback trace used by the integration plugins. */
void ResetTrace();
/** Appends one callback event to the shared trace. */
void Record(std::string_view event);
/** Returns a snapshot of all callback events in invocation order. */
std::vector<std::string> Trace();
/** Replaces the simulated active configuration for a named plugin. */
void SetActive(std::string_view plugin, std::string_view configuration);
/** Returns the simulated active configuration for a named plugin. */
std::string Active(std::string_view plugin);

}  // namespace dangd::test_plugin

#endif  // DANGD_TEST_PLUGINS_PLUGIN_TEST_SUPPORT_H_
