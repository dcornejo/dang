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

The TLS listeners poll idle connections at a bounded interval and call the
adapter even when the peer sends no new bytes. This is required for live RFC
5277 notifications, confirmed-commit timers, cancellation, and inactivity
deadlines; a blocking read must not postpone server-originated output.

Hosts should install `NetconfServer::SetRecoveryAuditSink` whenever recovery
users are configured. The sink receives a record before each recovery-user RPC
is parsed, including malformed attempts. It contains only session ID, username,
and request byte count; the host is responsible for timestamps, durable audit
storage, retention, and access control. `dangd` installs this hook and writes
safely encoded records to its diagnostic stream. Recovery RPC processing fails
closed with `operation-failed` if the sink throws.

Dangd installs `dangd-superuser` in this recovery set by default. This is a
NETCONF identity only, not a Unix account or a credential. For SSH, bind its
public key with `--ssh-authorized-key dangd-superuser=FILE`. For TLS, issue a
trusted client certificate whose selected identity is `dangd-superuser`, or
map a certificate identity explicitly with
`--username-map CERTIFICATE_NAME=dangd-superuser`. Credential rotation remains
a transport operation and takes effect on restart or SIGHUP. Disable the name
with `--no-default-superuser` only after testing another recovery identity;
ordinary NACM configuration cannot silently remove host recovery access.

For SSH, set `transport` to `kSsh`, preserve the SSH-authenticated username,
and set `ssh_subsystem` to the requested subsystem. The adapter rejects every
subsystem except the exact RFC 6242 name `netconf`. For TLS, the host performs
mutual certificate validation and RFC 7589 certificate-to-name mapping before
setting `peer_authenticated`; the library intentionally does not parse or
trust certificates itself.

`dangd` selects the embedded-host architecture. Libssh exclusively owns the
SSH listener, key exchange, host key, public-key authentication, session
channel, and subsystem exchange; `NetconfTransportAdapter` exclusively owns
NETCONF framing and lifecycle. There is no sidecar with overlapping framing or
identity responsibility. Password, keyboard-interactive, shell, exec, PTY,
forwarding, and non-session channels are not accepted.

Each accepted SSH connection runs in an independent worker so a client blocked
on authentication, input, or output cannot stop other NETCONF sessions. The
listener admits at most 64 simultaneous workers by default; use
`--ssh-max-sessions COUNT` to select a deployment-specific nonzero ceiling.
Finished workers are reaped continuously. SIGHUP waits for current workers to
finish before replacing the application, preventing a session from retaining
references into the prior schema or plugin runtime.

Each repeatable `--ssh-authorized-key USER=PUBLIC_KEY` entry is an explicit
local authorization record. `--ssh-group USER=GROUP` attaches a trusted NACM
external group to that record. The SSH username is passed through the same
repeatable `--username-map AUTHENTICATED=LOCAL` table as TLS; with
`--require-username-map`, unmapped authenticated users are rejected. The host
private key is supplied separately with `--ssh-host-key`.

`dangd --tls-username-source cn|san-dns|san-uri` selects the certificate field
used verbatim as the NETCONF/NACM username. The default is `cn`. The chosen
field must contain exactly one safe value; ambiguity or absence fails closed.
Values of other SAN types do not participate in the selected mapping.

`--username-map AUTHENTICATED=LOCAL` applies an exact mapping after certificate
field selection. Repeat it for several identities. Without
`--require-username-map`, an identity with no rule is used unchanged; with that
flag it is rejected. Duplicate authenticated names, empty/unsafe values, and
oversized names invalidate the complete map. Embedding SSH implementations can
use the same `MapAuthenticatedUsername` function after SSH authentication.

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
