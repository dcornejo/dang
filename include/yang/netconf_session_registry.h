// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_SESSION_REGISTRY_H_
#define YANG_NETCONF_SESSION_REGISTRY_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace yang::netconf {

/** Public information about one active NETCONF session. */
struct SessionInfo {
  std::uint32_t id = 0;
  std::string username;
  bool close_requested = false;
};

/** Thread-safe registry and shutdown-signaling boundary for NETCONF sessions. */
class SessionRegistry {
 public:
  /** Registers a nonzero, previously unused server-assigned session ID. */
  [[nodiscard]] bool Register(std::uint32_t id, std::string username);
  /** Removes a session. Returns false when it was not registered. */
  bool Unregister(std::uint32_t id);
  /** Marks a target session for asynchronous transport termination. */
  [[nodiscard]] bool RequestClose(std::uint32_t id);
  /** Returns whether the host must terminate this session's transport. */
  [[nodiscard]] bool CloseRequested(std::uint32_t id) const;
  /** Returns information for an active session, if present. */
  [[nodiscard]] std::optional<SessionInfo> Find(std::uint32_t id) const;
  /** Returns a stable snapshot of all active sessions. */
  [[nodiscard]] std::vector<SessionInfo> List() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::uint32_t, SessionInfo> sessions_;
};

}  // namespace yang::netconf

#endif  // YANG_NETCONF_SESSION_REGISTRY_H_
