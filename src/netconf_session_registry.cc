// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/netconf_session_registry.h"

#include <utility>

namespace yang::netconf {

bool SessionRegistry::Register(std::uint32_t id, std::string username) {
  if (id == 0 || username.empty()) return false;
  std::lock_guard lock(mutex_);
  return sessions_.emplace(id, SessionInfo{id, std::move(username), false})
      .second;
}

bool SessionRegistry::Unregister(std::uint32_t id) {
  std::lock_guard lock(mutex_);
  return sessions_.erase(id) != 0;
}

bool SessionRegistry::RequestClose(std::uint32_t id) {
  std::lock_guard lock(mutex_);
  const auto iterator = sessions_.find(id);
  if (iterator == sessions_.end()) return false;
  iterator->second.close_requested = true;
  return true;
}

bool SessionRegistry::CloseRequested(std::uint32_t id) const {
  std::lock_guard lock(mutex_);
  const auto iterator = sessions_.find(id);
  return iterator != sessions_.end() && iterator->second.close_requested;
}

std::optional<SessionInfo> SessionRegistry::Find(std::uint32_t id) const {
  std::lock_guard lock(mutex_);
  const auto iterator = sessions_.find(id);
  if (iterator == sessions_.end()) return std::nullopt;
  return iterator->second;
}

std::vector<SessionInfo> SessionRegistry::List() const {
  std::lock_guard lock(mutex_);
  std::vector<SessionInfo> result;
  result.reserve(sessions_.size());
  for (const auto& [id, session] : sessions_) {
    (void)id;
    result.push_back(session);
  }
  return result;
}

}  // namespace yang::netconf
