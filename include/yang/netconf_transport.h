// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_TRANSPORT_H_
#define YANG_NETCONF_TRANSPORT_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "yang/netconf_framing.h"

namespace yang::netconf {

enum class SecureTransport { kSsh, kTls };
enum class WriteStatus { kAccepted, kWouldBlock, kClosed, kFailed };

/** Authenticated information supplied by an SSH or TLS implementation. */
struct TransportIdentity {
  SecureTransport transport = SecureTransport::kSsh;
  std::string username;
  std::vector<std::string> external_groups;
  /** SSH subsystem name; ignored for TLS. */
  std::string ssh_subsystem;
  /** True only after host-key/certificate and peer authentication succeeds. */
  bool peer_authenticated = false;
  /** True only when external_groups came from the authenticated peer source. */
  bool external_groups_trusted = false;
};

/** Nonblocking secure byte-stream boundary implemented by the host. */
class SecureByteStream {
 public:
  virtual ~SecureByteStream() = default;
  [[nodiscard]] virtual WriteStatus Write(std::string_view bytes) = 0;
  virtual void Close() = 0;
};

/** Deterministic connection resource and inactivity policy. */
struct TransportLimits {
  std::size_t maximum_message_size = 16 * 1024 * 1024;
  std::size_t maximum_queued_messages = 1024;
  std::size_t maximum_queued_bytes = 16 * 1024 * 1024;
  std::chrono::seconds inactivity_timeout = std::chrono::minutes(10);
};

/** Event-loop adapter connecting one secure stream to a NetconfSession. */
class NetconfTransportAdapter {
 public:
  using Clock = std::chrono::steady_clock;
  NetconfTransportAdapter(NetconfServer& server, SecureByteStream& stream,
      std::uint32_t session_id, TransportIdentity identity,
      TransportLimits limits = {}, Clock::time_point now = Clock::now());
  ~NetconfTransportAdapter();
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::optional<std::string_view> error() const noexcept;
  /** Supplies decrypted bytes from the secure transport. */
  void Receive(std::string_view bytes, Clock::time_point now = Clock::now());
  /** Retries queued writes and processes notifications, cancellation, timeout. */
  void Poll(Clock::time_point now = Clock::now());
  /** Cancels and closes this connection. Safe to call repeatedly. */
  void Cancel();
  /** Reports EOF/error from the host transport and releases session resources. */
  void TransportClosed();
  [[nodiscard]] std::size_t queued_bytes() const noexcept;

 private:
  void Enqueue(std::string bytes);
  void Flush();
  void Consume(SessionOutput output);
  void Fail(std::string message);
  SecureByteStream& stream_;
  TransportLimits limits_;
  std::optional<NetconfSession> session_;
  std::vector<std::string> queue_;
  std::size_t queued_bytes_ = 0;
  Clock::time_point last_activity_;
  std::optional<std::string> error_;
  bool closed_ = false;
};

/** Call Home target supplied to a host connection factory. */
struct CallHomeTarget {
  std::string host;
  std::uint16_t port = 0;
  SecureTransport transport = SecureTransport::kSsh;
};

/** Host hook that initiates TCP then establishes SSH-server/TLS-server roles. */
using CallHomeConnector = std::function<std::unique_ptr<SecureByteStream>(
    const CallHomeTarget&, std::string* error)>;

/** Validates a Call Home target and invokes the host connector. */
[[nodiscard]] std::unique_ptr<SecureByteStream> ConnectCallHome(
    CallHomeTarget target, const CallHomeConnector& connector,
    std::string* error);

}  // namespace yang::netconf
#endif  // YANG_NETCONF_TRANSPORT_H_
