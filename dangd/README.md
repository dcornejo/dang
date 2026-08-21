<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# dangd

`dangd` is the host-application layer for a future production NETCONF
configuration server. It is kept separate from the reusable `yang` library.

The current foundation:

- loads one root YANG module and its import/include dependency closure;
- binds and completely validates an initial XML configuration;
- constructs running, candidate, and startup NETCONF datastores;
- replaces a backend working configuration whenever NETCONF replaces the
  running datastore and reports the schema-aware changes in plain English;
- optionally restores and saves an atomic datastore snapshot;
- provides `--check` startup validation; and
- provides an RFC 6242 stdin/stdout session for supervised integration tests.

Build and validate a configuration:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/dangd --model models/appliance.yang --config config.xml --check
```

The `--stdio` mode exercises a complete framed NETCONF session, but it does not
authenticate or encrypt the peer. The supplied `--username` is trusted. Do not
connect this mode directly to a socket; use it only in tests or behind a local
supervisor that has already authenticated the peer.

An event-loop production transport, richer operational device state, and a
device-specific backend remain future `dangd` work. The current English
backend is deliberately in-memory: it establishes the commit boundary and
delta vocabulary without pretending to configure an external system.

## RFC 8344 IP-management plugin example

The `dangd_ip_management_plugin` build target is a self-contained example
provider for RFC 8343 interfaces and RFC 8344 IP configuration. It discovers
both normative models through the plugin ABI and turns committed interface,
IPv4, and IPv6 changes into human-readable apply actions. The example prints
each action to the daemon's diagnostic stream and assumes success; it does not
modify host networking. See the concrete request and build instructions in
[the plugin guide](../docs/DANGD_PLUGINS.md).

## Safe hardware application ordering

A valid final configuration does not imply that every transition to it is
safe. For example, an interface and its ACL references may satisfy every YANG
constraint after a commit, while applying the interface's enabled state before
programming and attaching the ACL would briefly expose unfiltered traffic.
The deterministic, path-sorted configuration delta is therefore a reporting
format, not a hardware execution order.

Safe application should be divided into three layers:

1. **Desired-state validation.** YANG describes the valid final state. A
   `leafref` can require an interface's referenced ACL to exist, a `must`
   expression can require protection when an interface is enabled, and
   features, deviations, and constraints such as `max-elements` can describe
   static platform capabilities.
2. **Platform preflight.** Before changing hardware, the backend checks whether
   the complete transition is feasible. This includes dynamic limits such as
   available ACL or TCAM entries, whether old and new resources can coexist,
   atomic-swap support, and expected disruption. Dynamic resource availability
   belongs in operational state and backend policy rather than fixed YANG
   constraints. A failed preflight must leave hardware and the NETCONF running
   configuration unchanged.
3. **Ordered execution.** The backend converts the before/after configurations
   into actions connected by dependencies. It topologically orders that graph
   instead of executing the raw delta order.

The action vocabulary should distinguish creating, populating, binding,
activating, deactivating, unbinding, and destroying resources. Activation is
last and deactivation is first. An ACL-protected interface would normally be
applied as:

```text
create ACL -> program rules -> attach ACL -> enable interface
```

Removal reverses the safety boundary:

```text
disable or block interface -> detach ACL -> remove ACL
```

On hardware with staging support, replacement can program a new ACL in an
inactive slot, atomically switch the interface binding, and then remove the old
ACL.

Some dependencies are generic and can be inferred from the schema and trees:
parents precede children on creation, children precede parents on deletion,
referenced objects precede referring objects, and obsolete targets remain until
new references are attached. Other dependencies are platform-specific, such as
ACL programming before interface activation, VLAN creation before port
membership, or routing policy installation before enabling a peer. Those rules
belong in the device backend or a backend policy module. Optional YANG
extensions may annotate lifecycle roles, but should provide planning metadata
rather than attempt to encode an imperative hardware program.

A production backend boundary will consequently need two explicit stages:

```text
plan(before, after, changes) -> execution plan or preflight error
apply(plan)                  -> success or failure with rollback status
```

The plan must record dependencies, preconditions, rollback actions,
reversibility, disruption, and module/path context for failures. The running
datastore advances only after successful hardware application. On failure, the
backend rolls back completed actions; if rollback is incomplete, the NETCONF
error must explicitly report possible divergence between hardware and the
intended configuration.

## Mutual-TLS console example

`dangd` can expose its NETCONF session over a blocking mutual-TLS listener.
The server validates client certificates against the configured CA and maps
the verified certificate common name to the NETCONF username used by NACM.
The example certificate for `alice` therefore selects the `alice` rules in
`examples/nacm.xml`.

The material under `testdata/tls` is public, test-only cryptographic material.
Never deploy those keys or trust their CA outside a local demonstration.

Start the server from the repository root:

```sh
./build/dangd \
  --model dangd/examples/appliance.yang \
  --config dangd/examples/config.xml \
  --search dangd/models \
  --nacm dangd/examples/nacm.xml \
  --tls-listen 127.0.0.1 --tls-port 6513 \
  --tls-cert dangd/testdata/tls/server-cert.pem \
  --tls-key dangd/testdata/tls/server-key.pem \
  --tls-ca dangd/testdata/tls/ca-cert.pem
```

In another terminal window, start the deliberately simple client:

```sh
./build/dangctl \
  --host localhost --port 6513 \
  --cert dangd/testdata/tls/alice-cert.pem \
  --key dangd/testdata/tls/alice-key.pem \
  --ca dangd/testdata/tls/ca-cert.pem
```

The client prints the server hello. Paste one XML RPC, then enter a blank line:

```xml
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
  <get-config><source><running/></source></get-config>
</rpc>
```

`dangctl` frames the document, sends it over TLS, and prints the decoded
`rpc-reply`. It negotiates NETCONF base 1.0 so the interactive boundary remains
easy to see. End the session with `<close-session/>` or EOF.

The listener requires TLS 1.2 or newer, a trusted client certificate, and a
nonempty certificate common name. The client requires a trusted server chain
and verifies `--host` against the server certificate. The current CN mapping is
an explicit demonstration policy, not the configurable certificate-to-name
mapping defined by RFC 7589. The listener handles connections synchronously and
is intended for local integration and tests; production work still needs an
event-loop TLS service, revocation policy, configurable identity mapping,
operational monitoring, and protected deployment credentials.

## Managed NACM and plugins

`ietf-netconf-acm` is a core `dangd` model. `--nacm FILE` seeds the initial
datastore only when the configuration does not already contain `/nacm`; it is
not a permanent override. When `--state-file` names a file that does not yet
exist, this seeded state is durably saved before startup succeeds. Thereafter
authorized clients manage NACM through
ordinary candidate edits and commits. The policy active at RPC start
authorizes the change, and its compiled replacement becomes active only after
the complete backend transaction succeeds. When the NACM container is absent,
the RFC 8341 defaults remain active: reads and operations are permitted, while
configuration writes are denied. Configure at least one repeatable
`--recovery-user USER` whose authenticated sessions may bypass NACM to install
or repair policy. Recovery identities are host configuration, are never read
from the datastore, and survive NACM commits and `SIGHUP` reloads. Denial
counters remain core-owned operational state and are returned by `<get>`.

Application RPCs and YANG 1.1 actions are resolved against the compiled schema
before dispatch. `dangd` applies operation rules and `default-deny-all`; actions
also require read access to every data ancestor. Only then is the request sent
to the plugin that owns the defining module. Successful plugin output is NACM
read-filtered before it is returned.

POSIX plugins are loaded with repeatable `--plugin FILE` arguments. A plugin
supplies implemented, deviation, and import-only YANG source bytes. `dangd`
compiles them into the common effective schema and advertises the resulting
inventory through the RFC 8525 `/yang-library` operational tree returned by
`<get>`.

Send `SIGHUP` to a TLS-mode `dangd` process after replacing a configured
plugin or model file. The daemon stages and validates the entire replacement
against the current running configuration. On success it publishes an RFC
8525 `yang-library-update`; on failure it logs the diagnostics and continues
with the old schema. A connected subscriber receives the update before its
session is closed and should reconnect to negotiate the replacement library.

Model source is available directly over NETCONF using RFC 6022:

```xml
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="schema">
  <get-schema
      xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring">
    <identifier>appliance</identifier>
    <format>yang</format>
  </get-schema>
</rpc>
```

`version` may be supplied to select a revision. YANG is the default and only
format currently served. Unknown modules and formats return `invalid-value`;
an omitted version that matches multiple revisions returns
`data-not-unique`.

Configuration is coordinated by preparing all affected plugins, validating
all of them, applying in dependency order, and releasing preparations. Partial
apply failures roll already-applied plugins back in reverse order. Confirmed
commit cancellation and expiry also pass through the backend so plugin state
tracks the restored running datastore.

See [Writing a dangd configuration plugin](../docs/DANGD_PLUGINS.md) for the
complete ABI, ownership, dependency, transaction, rollback, error, threading,
and security contract. The reference implementation is
`dangd/plugins/example_plugin.cc`.
