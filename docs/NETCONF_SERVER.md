<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# NETCONF XML RPC service

`yang/netconf_server.h` connects unframed NETCONF XML messages to the datastore
manager. The host assigns and authenticates sessions, sends the server hello,
removes RFC 6242 transport framing, and passes each complete `<rpc>` document to
`NetconfServer::Process`.

```cpp
#include <yang/netconf_server.h>

yang::netconf::DatastoreManager stores(runtime_schema, initial_config);
yang::netconf::NetconfServer server(stores);

send_xml(server.ServerHello(1001));

auto reply = server.Process("1001", received_rpc_xml);
send_xml(reply.xml);
if (reply.close_session) {
  close_transport();
}
```

The server advertises NETCONF base 1.0 and 1.1, writable-running, candidate,
startup, validate 1.1, rollback-on-error, and confirmed-commit 1.1. It dispatches:

- `get` and `get-config` with subtree or XPath filters;
- `edit-config`, including default, test, and error options;
- `lock` and `unlock`;
- `validate`;
- `commit`, persistent and non-persistent confirmed-commit sequences, and
  confirmation;
- `cancel-commit` and `discard-changes`;
- `copy-config` from a datastore or complete inline `config`, and startup
  `delete-config`;
- `close-session` and `kill-session`.

When the RFC 8526 modules are present, `<get-data>` and `<edit-data>` inputs are
validated against their enabled YANG schema before execution. This prevents
unknown or duplicate parameters from being silently ignored and treats opaque
`anydata` configuration/filter payloads as content to be interpreted by the
operation rather than as children of the operation schema. If the NMDA
`with-defaults` feature is not enabled, `<get-data>` rejects that parameter with
`invalid-value`.

Replies preserve and XML-escape `message-id`, return one or more `rpc-error`
elements, and carry existing validator instance paths where available.

The server owns a thread-safe active-session registry. `NetconfSession`
registers its server-assigned ID, and `kill-session` rejects the caller's own ID
or an inactive ID with `invalid-value`. A successful kill immediately releases
the target's datastore locks and rolls back its non-persistent confirmed commit,
then marks its transport for closure. Authentication and I/O remain host-layer
work. This split lets the same service run over SSH, TLS, a test harness, or an
embedded management channel without placing networking policy inside the
schema library.

Inline `copy-config` sources are parsed as complete configurations and replace
the target atomically. The same schema validation, datastore locks, and NACM
create/update/delete checks used by edit transactions apply. Edit-control
attributes are rejected in an inline source, and identical source and target
datastores return `invalid-value`, as required by RFC 6241.

For datastore sources, read-denied nodes are silently removed before the
complete target replacement is validated and authorized. The RFC 8341
running-to-startup special case bypasses per-node read and write checks; only
permission to execute `copy-config` is required.

URL datastores are enabled only when the host supplies a
`UrlDatastoreProvider`. Its scheme allowlist is advertised in the `:url`
capability, and unsupported schemes are rejected before invoking the provider.
The provider owns authentication, I/O, size limits, atomic writes, and secret
handling. URL sources and targets are supported by `copy-config`, and URL
targets are supported by `delete-config`. URL payloads can also supply
`edit-config` input and `validate` sources. Provider-returned documents are
schema-validated before use or forwarding. Remote-to-remote copies are
supported when both URLs use allowed schemes; identical URLs are rejected.
Providers can return a precise NETCONF error tag with an error message.
Remote source-read and NACM target-inspection failures stop before the provider
write call. The provider's atomic-write contract requires a reported write
failure to retain the previous target content; such I/O failures are not NACM
denials and do not affect NACM counters.
