// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_SSH_TRANSPORT_H_
#define DANGD_SSH_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "yang/netconf_transport.h"

namespace dangd {

class Application;
struct ApplicationOptions;

/** One SSH username and public key authorized by the local server policy. */
struct SshAuthorizedUser {
  std::string username;
  std::filesystem::path public_key;
  /** NACM groups asserted by this authenticated server-side record. */
  std::vector<std::string> external_groups;
};

/** Embedded RFC 6242 SSH server settings. */
struct SshServerOptions {
  std::string address = "127.0.0.1";
  std::uint16_t port = 830;
  std::filesystem::path host_key;
  std::vector<SshAuthorizedUser> authorized_users;
  std::vector<yang::netconf::UsernameMapping> username_mappings;
  bool require_username_mapping = false;
  /** Zero serves until interrupted; nonzero supports deterministic tests. */
  std::size_t maximum_connections = 0;
  /** Maximum authenticated or handshaking clients processed simultaneously. */
  std::size_t maximum_concurrent_sessions = 64;
};

/** Runs the embedded public-key-only NETCONF-over-SSH server. */
[[nodiscard]] int RunReloadableSshServer(
    std::unique_ptr<Application>& application,
    const ApplicationOptions& application_options,
    const SshServerOptions& options, std::ostream& diagnostics);

}  // namespace dangd

#endif  // DANGD_SSH_TRANSPORT_H_
