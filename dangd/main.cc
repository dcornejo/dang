// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "dangd/application.h"
#include "dangd/ssh_transport.h"
#include "dangd/tls_transport.h"

namespace {

std::filesystem::path ExecutablePath(const char* argument) {
  for (const std::filesystem::path& candidate :
       {std::filesystem::path("/proc/self/exe"),
        std::filesystem::path("/proc/curproc/file")}) {
    std::error_code error;
    const auto linked = std::filesystem::read_symlink(candidate, error);
    if (!error && !linked.empty()) return linked;
  }
  std::error_code error;
  return std::filesystem::weakly_canonical(
      std::filesystem::absolute(argument, error), error);
}

void Usage() {
  std::cerr
      << "usage: dangd --model FILE --config FILE [--search DIR] [--state FILE]"
         " [--peer-journal FILE --peer-recovery FILE]"
         " [--peer-controller-user USER]..."
         " [--nacm FILE] [--recovery-user USER]... [--no-default-superuser]"
         " [--plugin FILE]..."
         " [--plugin-worker FILE]"
         " [--check | --stdio --username USER [--session-id ID]"
         " | --ssh-listen ADDRESS --ssh-port PORT --ssh-host-key FILE"
         " --ssh-authorized-key USER=FILE [--ssh-group USER=GROUP]..."
         " [--ssh-max-sessions COUNT]"
         " | --tls-listen ADDRESS --tls-port PORT --tls-cert FILE --tls-key "
         "FILE --tls-ca FILE [--tls-username-source cn|san-dns|san-uri]"
         " [--username-map AUTHENTICATED=LOCAL]... [--require-username-map]]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  dangd::ApplicationOptions options;
  const std::filesystem::path executable = ExecutablePath(argv[0]);
  if (!executable.empty()) {
    options.search_paths.push_back(
        executable.parent_path().parent_path() / "share/doc/yang/dangd/models");
    const auto sibling_worker = executable.parent_path() / "dangd-plugin-worker";
    std::error_code sibling_error;
    if (std::filesystem::is_regular_file(sibling_worker, sibling_error) &&
        !sibling_error) {
      options.plugin_worker_executable = sibling_worker;
    } else {
      options.plugin_worker_executable =
          executable.parent_path().parent_path() /
          "libexec/dangd/dangd-plugin-worker";
    }
  }
  bool stream_mode = false;
  bool ssh_mode = false;
  bool tls_mode = false;
  std::string username;
  std::uint32_t session_id = 1;
  dangd::TlsServerOptions tls;
  dangd::SshServerOptions ssh;
  std::vector<std::pair<std::string, std::string>> ssh_groups;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--model" && index + 1 < argc) {
      options.model = argv[++index];
    } else if (argument == "--config" && index + 1 < argc) {
      options.configuration = argv[++index];
    } else if (argument == "--search" && index + 1 < argc) {
      options.search_paths.emplace_back(argv[++index]);
    } else if (argument == "--state" && index + 1 < argc) {
      options.state_file = std::filesystem::path(argv[++index]);
    } else if (argument == "--peer-journal" && index + 1 < argc) {
      options.peer_transaction_journal = std::filesystem::path(argv[++index]);
    } else if (argument == "--peer-recovery" && index + 1 < argc) {
      options.peer_recovery_configuration =
          std::filesystem::path(argv[++index]);
    } else if (argument == "--peer-controller-user" && index + 1 < argc) {
      options.peer_controller_users.emplace_back(argv[++index]);
    } else if (argument == "--nacm" && index + 1 < argc) {
      options.nacm_configuration = std::filesystem::path(argv[++index]);
    } else if (argument == "--recovery-user" && index + 1 < argc) {
      options.recovery_users.emplace_back(argv[++index]);
    } else if (argument == "--no-default-superuser") {
      options.default_superuser = false;
    } else if (argument == "--plugin" && index + 1 < argc) {
      options.plugins.emplace_back(argv[++index]);
    } else if (argument == "--plugin-worker" && index + 1 < argc) {
      options.plugin_worker_executable = std::filesystem::path(argv[++index]);
    } else if (argument == "--username" && index + 1 < argc) {
      username = argv[++index];
    } else if (argument == "--session-id" && index + 1 < argc) {
      try {
        const std::string value = argv[++index];
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed);
        if (consumed != value.size()) throw std::invalid_argument("id");
        if (parsed == 0 || parsed > UINT32_MAX) throw std::out_of_range("id");
        session_id = static_cast<std::uint32_t>(parsed);
      } catch (const std::exception&) {
        Usage();
        return 2;
      }
    } else if (argument == "--stdio") {
      stream_mode = true;
      ssh_mode = false;
      tls_mode = false;
    } else if (argument == "--ssh-listen" && index + 1 < argc) {
      ssh.address = argv[++index];
      ssh_mode = true;
      stream_mode = false;
      tls_mode = false;
    } else if (argument == "--ssh-port" && index + 1 < argc) {
      try {
        const unsigned long port = std::stoul(argv[++index]);
        if (port == 0 || port > UINT16_MAX) throw std::out_of_range("port");
        ssh.port = static_cast<std::uint16_t>(port);
      } catch (const std::exception&) {
        Usage();
        return 2;
      }
    } else if (argument == "--ssh-host-key" && index + 1 < argc) {
      ssh.host_key = argv[++index];
    } else if (argument == "--ssh-max-sessions" && index + 1 < argc) {
      try {
        const std::string value = argv[++index];
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(value, &consumed);
        if (value.empty() || value.find_first_not_of("0123456789") !=
                                 std::string::npos ||
            consumed != value.size() || parsed == 0 ||
            parsed > std::numeric_limits<std::size_t>::max()) {
          throw std::out_of_range("sessions");
        }
        ssh.maximum_concurrent_sessions = static_cast<std::size_t>(parsed);
      } catch (const std::exception&) {
        Usage();
        return 2;
      }
    } else if (argument == "--ssh-authorized-key" && index + 1 < argc) {
      const std::string authorization = argv[++index];
      const std::size_t separator = authorization.find('=');
      if (separator == std::string::npos || separator == 0 ||
          separator + 1 == authorization.size()) {
        Usage();
        return 2;
      }
      ssh.authorized_users.push_back(
          {authorization.substr(0, separator),
           authorization.substr(separator + 1), {}});
    } else if (argument == "--ssh-group" && index + 1 < argc) {
      const std::string membership = argv[++index];
      const std::size_t separator = membership.find('=');
      if (separator == std::string::npos || separator == 0 ||
          separator + 1 == membership.size()) {
        Usage();
        return 2;
      }
      ssh_groups.emplace_back(membership.substr(0, separator),
                              membership.substr(separator + 1));
    } else if (argument == "--tls-listen" && index + 1 < argc) {
      tls.address = argv[++index];
      tls_mode = true;
      ssh_mode = false;
      stream_mode = false;
    } else if (argument == "--tls-port" && index + 1 < argc) {
      try {
        const unsigned long port = std::stoul(argv[++index]);
        if (port == 0 || port > UINT16_MAX) throw std::out_of_range("port");
        tls.port = static_cast<std::uint16_t>(port);
      } catch (const std::exception&) {
        Usage();
        return 2;
      }
    } else if (argument == "--tls-cert" && index + 1 < argc) {
      tls.certificate = argv[++index];
    } else if (argument == "--tls-key" && index + 1 < argc) {
      tls.private_key = argv[++index];
    } else if (argument == "--tls-ca" && index + 1 < argc) {
      tls.trust_anchor = argv[++index];
    } else if (argument == "--tls-username-source" && index + 1 < argc) {
      const std::string source = argv[++index];
      if (source == "cn") {
        tls.username_source = dangd::TlsUsernameSource::kCommonName;
      } else if (source == "san-dns") {
        tls.username_source = dangd::TlsUsernameSource::kSanDns;
      } else if (source == "san-uri") {
        tls.username_source = dangd::TlsUsernameSource::kSanUri;
      } else {
        Usage();
        return 2;
      }
    } else if (argument == "--username-map" && index + 1 < argc) {
      const std::string mapping = argv[++index];
      const std::size_t separator = mapping.find('=');
      if (separator == std::string::npos) {
        Usage();
        return 2;
      }
      tls.username_mappings.push_back(
          {mapping.substr(0, separator), mapping.substr(separator + 1)});
      ssh.username_mappings.push_back(
          {mapping.substr(0, separator), mapping.substr(separator + 1)});
    } else if (argument == "--require-username-map") {
      tls.require_username_mapping = true;
      ssh.require_username_mapping = true;
    } else if (argument == "--check") {
      stream_mode = false;
      ssh_mode = false;
      tls_mode = false;
    } else {
      Usage();
      return 2;
    }
  }
  if (options.model.empty() || options.configuration.empty() ||
      (options.peer_recovery_configuration &&
       !options.peer_transaction_journal) ||
      (!options.plugins.empty() && !options.plugin_worker_executable) ||
      (stream_mode && username.empty()) ||
      (ssh_mode &&
       (ssh.host_key.empty() || ssh.authorized_users.empty())) ||
      (tls_mode && (tls.certificate.empty() || tls.private_key.empty() ||
                    tls.trust_anchor.empty()))) {
    Usage();
    return 2;
  }
  for (const auto& [user, group] : ssh_groups) {
    const auto found = std::ranges::find(
        ssh.authorized_users, user, &dangd::SshAuthorizedUser::username);
    if (found == ssh.authorized_users.end()) {
      Usage();
      return 2;
    }
    found->external_groups.push_back(group);
  }

  auto loaded = dangd::Application::Load(options);
  if (!loaded.application) {
    for (const std::string& error : loaded.errors)
      std::cerr << "dangd: " << error << '\n';
    return 1;
  }
  if (tls_mode)
    return dangd::RunReloadableTlsServer(loaded.application, options, tls,
                                         std::cerr);
  if (ssh_mode)
    return dangd::RunReloadableSshServer(loaded.application, options, ssh,
                                         std::cerr);
  if (!stream_mode) {
    std::cout << "dangd: configuration is valid\n";
    return 0;
  }
  std::cerr << "dangd: warning: --stdio trusts the supplied username; "
               "use only behind an authenticated local supervisor\n";
  return dangd::RunStreamSession(*loaded.application, std::cin, std::cout,
                                 std::cerr, session_id, std::move(username));
}
