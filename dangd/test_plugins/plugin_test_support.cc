// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/test_plugins/plugin_test_support.h"

#include <mutex>
#include <unordered_map>

namespace dangd::test_plugin {
namespace {

std::mutex mutex;
std::vector<std::string> trace;
std::unordered_map<std::string, std::string> active;

}  // namespace

void ResetTrace() {
  std::lock_guard lock(mutex);
  trace.clear();
  active.clear();
}

void Record(std::string_view event) {
  std::lock_guard lock(mutex);
  trace.emplace_back(event);
}

std::vector<std::string> Trace() {
  std::lock_guard lock(mutex);
  return trace;
}

void SetActive(std::string_view plugin, std::string_view configuration) {
  std::lock_guard lock(mutex);
  active[std::string(plugin)] = configuration;
}

std::string Active(std::string_view plugin) {
  std::lock_guard lock(mutex);
  return active[std::string(plugin)];
}

}  // namespace dangd::test_plugin
