// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/netconf_transport.h"

#include <ranges>
#include <set>
#include <utility>

namespace yang::netconf {
namespace {
bool ValidXmlText(std::string_view value) {
  if (value.empty()) return false;
  return std::ranges::all_of(value, [](unsigned char character) {
    return character == 0x09 || character == 0x0A || character == 0x0D ||
           character >= 0x20;
  });
}
}  // namespace

NetconfTransportAdapter::NetconfTransportAdapter(
    NetconfServer& server, SecureByteStream& stream, std::uint32_t session_id,
    TransportIdentity identity, TransportLimits limits, Clock::time_point now)
    : stream_(stream), limits_(limits), last_activity_(now) {
  if (!identity.peer_authenticated) {
    Fail("secure transport peer is not authenticated");
    return;
  }
  if (!ValidXmlText(identity.username)) {
    Fail("authenticated NETCONF username is not valid XML text");
    return;
  }
  if (!identity.external_groups.empty() &&
      !identity.external_groups_trusted) {
    Fail("external NACM groups lack trusted authentication provenance");
    return;
  }
  std::set<std::string_view> unique_groups;
  for (const std::string& group : identity.external_groups) {
    if (!ValidXmlText(group) || group.size() > 255 ||
        !unique_groups.insert(group).second) {
      Fail("external NACM groups must be unique, bounded valid XML text");
      return;
    }
  }
  if (identity.transport == SecureTransport::kSsh &&
      identity.ssh_subsystem != "netconf")
    { Fail("SSH session did not request the netconf subsystem"); return; }
  if (limits_.maximum_message_size == 0 ||
      limits_.maximum_queued_messages == 0 ||
      limits_.maximum_queued_bytes == 0 ||
      limits_.inactivity_timeout <= std::chrono::seconds::zero())
    { Fail("transport limits must be positive"); return; }
  session_.emplace(server, session_id, std::move(identity.username),
                   limits_.maximum_message_size);
  session_->set_external_groups(std::move(identity.external_groups));
  if (!session_->valid()) {
    Fail("NETCONF session ID is invalid or active");
    return;
  }
  Enqueue(session_->Start());
  Flush();
}

NetconfTransportAdapter::~NetconfTransportAdapter() { TransportClosed(); }
bool NetconfTransportAdapter::valid() const noexcept {
  return session_.has_value() && session_->valid() && !closed_;
}
std::optional<std::string_view> NetconfTransportAdapter::error() const noexcept {
  return error_ ? std::optional<std::string_view>(*error_) : std::nullopt;
}
std::size_t NetconfTransportAdapter::queued_bytes() const noexcept {
  return queued_bytes_;
}

void NetconfTransportAdapter::Receive(std::string_view bytes,
                                      Clock::time_point now) {
  if (!valid()) return;
  last_activity_ = now;
  Consume(session_->Receive(bytes));
  Flush();
}
void NetconfTransportAdapter::Poll(Clock::time_point now) {
  if (!valid()) return;
  if (now - last_activity_ >= limits_.inactivity_timeout)
    return Fail("NETCONF transport inactivity timeout");
  Consume(session_->Poll());
  Flush();
}
void NetconfTransportAdapter::Cancel() {
  if (!closed_) Fail("NETCONF transport cancelled");
}
void NetconfTransportAdapter::TransportClosed() {
  if (closed_) return;
  closed_ = true;
  if (session_) session_->TransportClosed();
}
void NetconfTransportAdapter::Enqueue(std::string bytes) {
  if (bytes.empty() || closed_) return;
  if (queue_.size() >= limits_.maximum_queued_messages ||
      queued_bytes_ + bytes.size() > limits_.maximum_queued_bytes) {
    return Fail("NETCONF transport output queue limit exceeded");
  }
  queued_bytes_ += bytes.size();
  queue_.push_back(std::move(bytes));
}
void NetconfTransportAdapter::Flush() {
  while (!queue_.empty() && !closed_) {
    const WriteStatus status = stream_.Write(queue_.front());
    if (status == WriteStatus::kWouldBlock) return;
    if (status == WriteStatus::kClosed)
      return Fail("secure transport closed while writing");
    if (status == WriteStatus::kFailed)
      return Fail("secure transport write failed");
    queued_bytes_ -= queue_.front().size();
    queue_.erase(queue_.begin());
  }
}
void NetconfTransportAdapter::Consume(SessionOutput output) {
  for (std::string& bytes : output.bytes_to_send) Enqueue(std::move(bytes));
  if (output.error) error_ = std::move(*output.error);
  if (output.close_transport) {
    // A successful close-session reply must reach the peer before TLS/SSH is
    // closed. Receive()'s normal trailing Flush cannot run after Fail().
    Flush();
    Fail(error_.value_or("NETCONF session closed"));
  }
}
void NetconfTransportAdapter::Fail(std::string message) {
  if (closed_) return;
  error_ = std::move(message);
  closed_ = true;
  if (session_) session_->TransportClosed();
  stream_.Close();
}

std::unique_ptr<SecureByteStream> ConnectCallHome(
    CallHomeTarget target, const CallHomeConnector& connector,
    std::string* error) {
  if (target.host.empty()) {
    if (error) *error = "Call Home target host is empty";
    return nullptr;
  }
  if (target.port == 0)
    target.port = target.transport == SecureTransport::kSsh ? 4334 : 4335;
  if (!connector) {
    if (error) *error = "Call Home connector is not configured";
    return nullptr;
  }
  return connector(target, error);
}
}  // namespace yang::netconf
