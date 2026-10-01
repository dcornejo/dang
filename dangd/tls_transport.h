// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_TLS_TRANSPORT_H_
#define DANGD_TLS_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/types.h>

#include "yang/netconf_transport.h"

namespace dangd {

class Application;
struct ApplicationOptions;

/** Certificate field selected as the authenticated NETCONF username. */
enum class TlsUsernameSource { kCommonName, kSanDns, kSanUri };

/** Mutual-TLS listener settings for the blocking demonstration server. */
struct TlsServerOptions {
  std::string address = "127.0.0.1";
  std::uint16_t port = 6513;
  std::filesystem::path certificate;
  std::filesystem::path private_key;
  std::filesystem::path trust_anchor;
  TlsUsernameSource username_source = TlsUsernameSource::kCommonName;
  std::vector<yang::netconf::UsernameMapping> username_mappings;
  bool require_username_mapping = false;
  /** Zero serves until interrupted; nonzero is useful for deterministic tests. */
  std::size_t maximum_connections = 0;
};

/** TLS client settings used by the interactive XML console. */
struct TlsClientOptions {
  std::string host = "localhost";
  std::uint16_t port = 6513;
  std::filesystem::path certificate;
  std::filesystem::path private_key;
  std::filesystem::path trust_anchor;
  /** Bounds each socket connect, TLS I/O, write, and reply-read wait. */
  std::uint32_t timeout_milliseconds = 10'000;
};

/** Authenticated server hello and reply returned by one NETCONF RPC. */
struct TlsRpcExchange {
  std::string server_hello;
  std::string reply;
};

/** Maps exactly one safe certificate subject common name to a NACM username. */
[[nodiscard]] std::optional<std::string> CertificateSubjectUsername(
    const X509_NAME* subject);
/** Maps exactly one safe value from the configured certificate field. */
[[nodiscard]] std::optional<std::string> CertificateUsername(
    const X509* certificate, TlsUsernameSource source);

/**
 * Serves authenticated NETCONF-over-TLS connections synchronously.
 *
 * Client certificates must chain to trust_anchor. Exactly one nonempty, bounded
 * value from the configured CN, DNS SAN, or URI SAN field becomes the NETCONF
 * username supplied to NACM. Returns after
 * maximum_connections successful TCP accepts, or on a listener failure.
 */
[[nodiscard]] int RunTlsServer(Application& application,
                               const TlsServerOptions& options,
                               std::ostream& diagnostics);
/** Runs TLS and atomically reloads the application on POSIX SIGHUP. */
[[nodiscard]] int RunReloadableTlsServer(
    std::unique_ptr<Application>& application,
    const ApplicationOptions& application_options,
    const TlsServerOptions& options, std::ostream& diagnostics);

/**
 * Runs a mutual-TLS NETCONF console.
 *
 * Reads XML documents separated by a blank line, frames them as NETCONF base
 * 1.0 messages, and writes each decoded server reply to output.
 */
[[nodiscard]] int RunTlsClient(const TlsClientOptions& options,
                               std::istream& input, std::ostream& output,
                               std::ostream& diagnostics);

/**
 * Executes one byte-bounded NETCONF RPC over a fresh mutual-TLS session.
 *
 * The server certificate and hostname are verified, both protocol documents
 * are parsed as untrusted XML, and the socket is closed before return.
 */
[[nodiscard]] std::optional<TlsRpcExchange> ExchangeTlsRpc(
    const TlsClientOptions& options, std::string_view rpc, std::string* error,
    std::span<const std::string_view> required_server_capabilities = {});

}  // namespace dangd

#endif  // DANGD_TLS_TRANSPORT_H_
