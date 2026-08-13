// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/configuration_backend.h"

#include <utility>

namespace dangd {
namespace {

std::string Quote(const std::optional<std::string>& value) {
  if (!value) return {};
  std::string result = "\"";
  for (const char character : *value) {
    if (character == '\\' || character == '"') result += '\\';
    result += character;
  }
  return result + "\"";
}

std::string Describe(const yang::config::ChangeEvent& change) {
  using yang::config::ChangeKind;
  if (change.kind == ChangeKind::kCreated) {
    std::string result = "Created " + change.instance_path;
    if (change.after) result += " with value " + Quote(change.after);
    return result + ".";
  }
  if (change.kind == ChangeKind::kDeleted) {
    std::string result = "Deleted " + change.instance_path;
    if (change.before) result += ", whose value was " + Quote(change.before);
    return result + ".";
  }
  if (change.kind == ChangeKind::kValueChanged) {
    return "Changed " + change.instance_path + " from " +
           Quote(change.before) + " to " + Quote(change.after) + ".";
  }
  return "Replaced the configuration subtree at " + change.instance_path +
         ".";
}

}  // namespace

void EnglishConfigurationBackend::Replace(
    const yang::config::RuntimeSchema&,
    const yang::config::ConfigDocument&,
    const yang::config::ConfigDocument& after,
    std::span<const yang::config::ChangeEvent> changes) {
  std::lock_guard lock(mutex_);
  for (const auto& change : changes) deltas_.push_back(Describe(change));
  working_ = after;
}

yang::config::ConfigDocument EnglishConfigurationBackend::Working() const {
  std::lock_guard lock(mutex_);
  return working_;
}

std::vector<std::string> EnglishConfigurationBackend::DrainDeltas() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> result = std::move(deltas_);
  deltas_.clear();
  return result;
}

}  // namespace dangd
