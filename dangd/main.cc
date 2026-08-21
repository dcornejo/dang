// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "dangd/application.h"
#include "dangd/tls_transport.h"

namespace {

void Usage() {
  std::cerr
      << "usage: dangd --model FILE --config FILE [--search DIR] [--state FILE]"
         " [--nacm FILE] [--recovery-user USER]... [--plugin FILE]... [--check | --stdio --username USER [--session-id ID]"
         " | --tls-listen ADDRESS --tls-port PORT --tls-cert FILE --tls-key "
         "FILE --tls-ca FILE [--tls-username-source cn|san-dns|san-uri]]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  dangd::ApplicationOptions options;
  std::error_code executable_error;
  const std::filesystem::path executable =
      std::filesystem::weakly_canonical(
          std::filesystem::absolute(argv[0], executable_error),
          executable_error);
  if (!executable_error) {
    options.search_paths.push_back(
        executable.parent_path().parent_path() / "share/doc/yang/dangd/models");
  }
  bool stream_mode = false;
  bool tls_mode = false;
  std::string username;
  std::uint32_t session_id = 1;
  dangd::TlsServerOptions tls;
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
    } else if (argument == "--nacm" && index + 1 < argc) {
      options.nacm_configuration = std::filesystem::path(argv[++index]);
    } else if (argument == "--recovery-user" && index + 1 < argc) {
      options.recovery_users.emplace_back(argv[++index]);
    } else if (argument == "--plugin" && index + 1 < argc) {
      options.plugins.emplace_back(argv[++index]);
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
      tls_mode = false;
    } else if (argument == "--tls-listen" && index + 1 < argc) {
      tls.address = argv[++index];
      tls_mode = true;
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
    } else if (argument == "--check") {
      stream_mode = false;
      tls_mode = false;
    } else {
      Usage();
      return 2;
    }
  }
  if (options.model.empty() || options.configuration.empty() ||
      (stream_mode && username.empty()) ||
      (tls_mode && (tls.certificate.empty() || tls.private_key.empty() ||
                    tls.trust_anchor.empty()))) {
    Usage();
    return 2;
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
  if (!stream_mode) {
    std::cout << "dangd: configuration is valid\n";
    return 0;
  }
  std::cerr << "dangd: warning: --stdio trusts the supplied username; "
               "use only behind an authenticated local supervisor\n";
  return dangd::RunStreamSession(*loaded.application, std::cin, std::cout,
                                 std::cerr, session_id, std::move(username));
}
