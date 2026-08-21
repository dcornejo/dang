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
#include <string>

#include <openssl/types.h>

namespace dangd {

class Application;
struct ApplicationOptions;

/** Mutual-TLS listener settings for the blocking demonstration server. */
struct TlsServerOptions {
  std::string address = "127.0.0.1";
  std::uint16_t port = 6513;
  std::filesystem::path certificate;
  std::filesystem::path private_key;
  std::filesystem::path trust_anchor;
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
};

/** Maps exactly one safe certificate subject common name to a NACM username. */
[[nodiscard]] std::optional<std::string> CertificateSubjectUsername(
    const X509_NAME* subject);

/**
 * Serves authenticated NETCONF-over-TLS connections synchronously.
 *
 * Client certificates must chain to trust_anchor. Exactly one nonempty, bounded
 * UTF-8 common name without surrounding whitespace, controls, or embedded NUL
 * becomes the NETCONF username supplied to NACM. Returns after
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

}  // namespace dangd

#endif  // DANGD_TLS_TRANSPORT_H_
