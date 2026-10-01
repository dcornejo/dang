<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# dangd

`dangd` is the repository's model-driven NETCONF configuration server. It is
kept separate from the reusable `yang` library so transport, identity,
persistence, plugin supervision, and deployment policy remain host concerns.

The server:

- loads one root YANG module and its import/include dependency closure;
- binds and completely validates an initial XML configuration;
- constructs running, candidate, and startup NETCONF datastores;
- replaces a backend working configuration whenever NETCONF replaces the
  running datastore and reports the schema-aware changes in plain English;
- optionally restores an atomic datastore snapshot and makes each live
  persistent mutation durable before reporting NETCONF success;
- provides `--check` startup validation;
- serves authenticated NETCONF over embedded SSH or mutual TLS;
- provides an RFC 6242 stdin/stdout session for supervised integration tests;
- enforces datastore-managed NACM and publishes YANG Library state; and
- loads device plugins into supervised worker processes.

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

Production deployment hardening, broader interoperability coverage, and full
standards closure remain active work. The built-in English backend remains a
safe in-memory demonstration, while device plugins can apply native platform
changes behind the same transaction boundary.

## RFC 8344 IP-management plugin example

The separately packaged `dang_plugins` `dangd_ip_management_plugin` is a self-contained example
provider for RFC 8343 interfaces and RFC 8344 IP configuration. It discovers
both normative models through the plugin ABI and turns committed interface,
IPv4, and IPv6 changes into human-readable apply actions. Linux uses direct
rtnetlink operations for link state, MTU, addresses, and static neighbors and
publishes live state for configured interfaces. FreeBSD uses interface ioctls
and route netlink for link state, MTU, addresses, and static neighbors, plus
live interface/IP operational publication. Neither backend creates or deletes
interfaces, so this remains a partial device implementation rather than a
complete RFC 8343/8344 claim.
Unsupported development hosts use a logging-only backend. See the concrete
request, privilege, and build instructions in
[the plugin guide](../docs/DANGD_PLUGINS.md) and its provider-specific README.

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

ABI v4 plugins expose retained, reversible actions with stable identifiers,
schema instance paths, explicit dependencies, and normal, activate, or
deactivate classes. The common planner rejects missing dependencies and cycles,
orders deactivation first and activation last, and infers parent-before-child
creation plus child-before-parent deactivation. An ACL-protected interface
would normally be applied as:

```text
create ACL -> program rules -> attach ACL -> enable interface
```

Removal reverses the safety boundary:

```text
disable or block interface -> detach ACL -> remove ACL
```

ABI v5 independently extends operational publication with an explicit
completeness assertion. It lets a provider state that omitted children of each
returned node are truly absent, enabling mandatory and required-reference
validation without imposing that assumption on older or partial providers.

ABI v6 adds post-apply state fidelity. After hardware actions succeed, affected
plugins run in dependency order and receive the complete snapshot accepted so
far. Each may return a schema-valid replacement plus unique per-node
`applied`, `transformed`, `rejected`, or `delayed` results. Dangd publishes only
the final accepted snapshot in `<operational>` and exposes the results through
the reconciliation model. Invalid XML, invalid dispositions, or duplicate path
claims fail closed and initiate transaction compensation.

ABI v7 adds exclusive resource-domain claims for conflicts that module names
cannot reveal. For example, an FRR-native provider and an RFC 8431 provider
expose different YANG modules but both control the host routing plane; both
claim `routing`, so dangd rejects the deployment during discovery before either
can apply configuration.

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

The implemented backend boundary has two explicit stages:

```text
plan(before, after, changes) -> execution plan or preflight error
apply(plan)                  -> success or failure with rollback status
```

Plugins perform dynamic resource preflight in side-effect-free prepare and
validate callbacks before the planner executes anything. The running datastore
advances only after every hardware action succeeds. On failure, the planner
rolls back completed actions in reverse execution order; incomplete rollback
returns the `hardware-state-diverged` app-tag and explicitly warns that hardware
may differ from running. Each failed compensation remains visible under the
modeled `dangd-reconciliation:hardware-reconciliation` operational tree until
a fully successful later hardware transaction. ABI v1-v3 plugins remain
supported as one reversible transaction action per plugin; their remnant path
is empty because the legacy callback has transaction-wide granularity.

Configuration spanning independent dangd servers requires a stronger durable
decision boundary than this local transaction. The implemented state machine,
fail-closed recovery rules, and confirmation-only mutual-TLS recovery adapter
are described in
[Peer transaction coordination](../docs/PEER_TRANSACTIONS.md). Configured peer
endpoints and lifecycle integration are not yet implemented.

## Embedded SSH server example

`dangd` embeds libssh and supports public-key-only NETCONF over SSH. The SSH
host authenticates the key before constructing a NETCONF identity, accepts only
the exact `netconf` subsystem, and obtains NACM external groups only from the
local authorized-key record.

The keys under `testdata/ssh` are repository-only test fixtures, including
private keys. They are excluded from installed packages. Never deploy them.

Start the server:

```sh
./build/dangd \
  --model dangd/examples/appliance.yang \
  --config dangd/examples/config.xml \
  --search dangd/models \
  --nacm dangd/examples/nacm.xml \
  --ssh-listen 127.0.0.1 --ssh-port 830 \
  --ssh-host-key dangd/testdata/ssh/host-key \
  --ssh-authorized-key alice=dangd/testdata/ssh/alice-key.pub \
  --ssh-group alice=administrators \
  --ssh-max-sessions 64
```

Connect with OpenSSH and request the required subsystem:

```sh
ssh -p 830 -i dangd/testdata/ssh/alice-key \
  -o IdentitiesOnly=yes -s alice@127.0.0.1 netconf
```

The server hello appears on standard output. Paste base 1.0 framed XML ending
in `]]>]]>`. Use production host/user keys, file permissions, algorithm policy,
logging, and supervision outside demonstrations. Send `SIGHUP` to request an
atomic application/model reload; a failed reload preserves the active service.
SSH sessions execute independently, subject to the nonzero concurrent-session
ceiling (64 by default), so a slow reader does not stall unrelated clients.

## Mutual-TLS console example

`dangd` can expose its NETCONF session over a blocking mutual-TLS listener.
The server validates client certificates against the configured CA. By default
it maps the verified certificate common name to the NETCONF username used by
NACM. `--tls-username-source san-dns` or `san-uri` instead selects a DNS or URI
subjectAltName. The selected field must contain exactly one nonempty value of
at most 255 bytes; surrounding whitespace, controls, embedded NUL, duplicates,
and missing values are rejected rather than normalized.
Repeat `--username-map AUTHENTICATED=LOCAL` to translate selected certificate
values to local NACM accounts. Add `--require-username-map` to reject any
verified identity without an exact mapping.
The example certificate for `alice` therefore selects the `alice` rules in
`examples/nacm.xml`.

The material under `testdata/tls` is repository-only test cryptographic
material, including private keys. It is excluded from installed packages.
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

The listener requires TLS 1.2 or newer, a trusted client certificate, and one
safe value in the configured username field. The client requires a trusted
server chain and verifies `--host` against the server certificate. Field
selection and exact local-account transformation are configurable. The listener
handles connections synchronously and is intended for
local integration and tests; production work still needs an event-loop TLS
service, revocation policy, production identity lifecycle management,
operational monitoring, and protected deployment credentials.

## Managed NACM and plugins

`ietf-netconf-acm` is a core `dangd` model. `--nacm FILE` seeds the initial
datastore only when the configuration does not already contain `/nacm`; it is
not a permanent override. When `--state` names a file that does not yet
exist, this seeded state is durably saved before startup succeeds. Thereafter
authorized clients manage NACM through
ordinary candidate edits and commits. The policy active at RPC start
authorizes the change, and its compiled replacement becomes active only after
the complete backend transaction succeeds. When the NACM container is absent,
the RFC 8341 defaults remain active: reads and operations are permitted, while
configuration writes are denied. The built-in `dangd-superuser` identity may
bypass NACM to install or repair policy once an SSH key or TLS certificate is
authenticated as that exact name. Dangd and its packages create no
operating-system account or credential for it. Use `--no-default-superuser`
after establishing and testing another repeatable `--recovery-user USER`.
Recovery identities are host configuration, are never read from the datastore,
and survive NACM commits and `SIGHUP` reloads. They must be
unique canonical UTF-8 names; padded, control-containing, embedded-NUL,
oversized, malformed, or duplicate identities make startup fail. Denial
counters remain core-owned operational state and are returned by `<get>`.

The state snapshot is server-owned and may contain modeled secrets. Dangd
creates it mode 0600 and restores only a regular, non-symlink file owned by its
effective user with no group or other access. It also holds an exclusive lock
on a private sibling named `FILE.lock` for the complete application lifetime;
a second instance configured with the same state path fails startup, while an
atomic `SIGHUP` reload inherits the existing lock. The lock file remains after
shutdown and must not be deleted while dangd is running. The backup and offline
restore procedure is documented in the user guide. Before an application can
accept a session, dangd activates the complete initial or restored running tree
through the affected plugins as one dependency-ordered transaction.

`--peer-journal FILE` reserves a separate private recovery path for future
pair-wide transactions. If the path is absent, startup continues. If it
contains an unsafe, malformed, or unresolved journal, startup and `SIGHUP`
reload fail closed before accepting the replacement application. Diagnostics
name the transaction and pending peers but do not expose proposal digests or
persistent confirmed-commit tokens. An authenticated mutual-TLS confirmation
adapter and private `--peer-recovery FILE` endpoint mapping exist, but automatic
lifecycle recovery is not yet wired. Configuring these options is therefore a
safety gate rather than enabling pair-wide commits. See the user guide for the
versioned JSON format.

Application RPCs and YANG 1.1 actions are resolved against the compiled schema
before dispatch. `dangd` applies operation rules and `default-deny-all`; actions
also require read access to every data ancestor. Only then is the request sent
to the plugin that owns the defining module. Successful plugin output is NACM
read-filtered before it is returned.

POSIX plugins are configured with repeatable `--plugin FILE` arguments. The
daemon loads each one only in its supervised `dangd-plugin-worker` process; use
`--plugin-worker FILE` to override the automatically resolved build-tree or
installed worker. A plugin supplies implemented, deviation, and import-only YANG
source bytes. `dangd`
compiles them into the common effective schema and advertises the resulting
inventory through the RFC 8525 `/yang-library` operational tree returned by
`<get>`.

Send `SIGHUP` to an SSH- or TLS-mode `dangd` process after replacing a configured
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
