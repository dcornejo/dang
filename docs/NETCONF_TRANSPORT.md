<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# SSH, TLS, and Call Home integration

`NetconfTransportAdapter` connects the protocol session to any nonblocking
secure byte stream without selecting an SSH, TLS, socket, or event-loop
library. The host completes secure transport authentication first and then
passes the resulting NETCONF identity unchanged.

```cpp
class MyTlsStream final : public yang::netconf::SecureByteStream {
 public:
  yang::netconf::WriteStatus Write(std::string_view bytes) override;
  void Close() override;
};

yang::netconf::TransportIdentity identity{
    .transport = yang::netconf::SecureTransport::kTls,
    .username = certificate_mapped_username,
    .external_groups = authenticated_groups,
    .peer_authenticated = certificate_chain_and_identity_are_valid,
    .external_groups_trusted = groups_came_from_the_authenticated_identity,
};

yang::netconf::TransportLimits limits;
limits.maximum_message_size = 16 * 1024 * 1024;
limits.maximum_queued_messages = 256;
limits.maximum_queued_bytes = 8 * 1024 * 1024;
limits.inactivity_timeout = std::chrono::minutes(10);

yang::netconf::NetconfTransportAdapter connection(
    server, tls_stream, allocated_session_id, std::move(identity), limits);

// Event-loop callbacks:
connection.Receive(decrypted_bytes);
connection.Poll();  // writable/timer/notification/cancellation processing
connection.TransportClosed();  // EOF or secure-transport failure
```

Hosts should install `NetconfServer::SetRecoveryAuditSink` whenever recovery
users are configured. The sink receives a record before each recovery-user RPC
is parsed, including malformed attempts. It contains only session ID, username,
and request byte count; the host is responsible for timestamps, durable audit
storage, retention, and access control. `dangd` installs this hook and writes
safely encoded records to its diagnostic stream. Recovery RPC processing fails
closed with `operation-failed` if the sink throws.

For SSH, set `transport` to `kSsh`, preserve the SSH-authenticated username,
and set `ssh_subsystem` to the requested subsystem. The adapter rejects every
subsystem except the exact RFC 6242 name `netconf`. For TLS, the host performs
mutual certificate validation and RFC 7589 certificate-to-name mapping before
setting `peer_authenticated`; the library intentionally does not parse or
trust certificates itself.

`dangd --tls-username-source cn|san-dns|san-uri` selects the certificate field
used verbatim as the NETCONF/NACM username. The default is `cn`. The chosen
field must contain exactly one safe value; ambiguity or absence fails closed.
Values of other SAN types do not participate in the selected mapping.

Nonempty `external_groups` are rejected unless `external_groups_trusted` is
true. Set it only when every group came from the authenticated transport
identity or another equally trusted authorization source—not from NETCONF
message content. Group names must be nonempty valid XML text, at most 255 bytes,
and unique within the session. The adapter copies each accepted group set into
its session, preventing later caller mutation or cross-session reuse.

`SecureByteStream::Write` is nonblocking. `kAccepted` means the implementation
has accepted ownership of the complete supplied buffer; `kWouldBlock` means it
accepted none. A library exposing partial writes should retain its own cursor
and return `kAccepted` only after it owns the complete buffer. The adapter
retries blocked buffers in order and closes the connection on write failure or
when configured queue limits are exceeded.

Inactivity is measured from the most recent received decrypted byte. Calling
`Poll` at or after the deadline closes the transport and releases NETCONF
locks, subscriptions, and non-persistent confirmed commits. `Cancel` provides
an idempotent cancellation path, and `TransportClosed` handles EOF or an
external secure-transport failure.

## Call Home

`ConnectCallHome` validates a target and invokes a host connection factory. A
zero port selects RFC 8071 port 4334 for SSH or 4335 for TLS. The factory first
initiates TCP, then establishes the device as SSH/TLS server over that outbound
connection. Only TCP initiation is reversed: secure-transport authentication
and NETCONF client/server roles remain unchanged. Host-key/certificate trust,
reconnect schedules, jitter, keepalives, DNS, and socket policy remain with the
embedding application.
