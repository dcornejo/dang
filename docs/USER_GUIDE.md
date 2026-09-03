<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# User guide

This guide explains how `yang-cpp` fits into an application and then uses
`dangd`, the repository's example NETCONF server, to show the pieces working
together. The focused documents in this directory remain the references for
individual subsystems.

## The implementation model

The library separates model processing, configuration processing, NETCONF
semantics, and host integration. Applications can stop at any layer or compose
all of them.

```text
YANG files
    |
    v
Compiler ---- diagnostics
    |
    v
RuntimeSchema
    |
    +---- ParseDatastoreXml + ConfigValidator ---- valid ConfigDocument
    |
    +---- ConfigEditor --------------------------- candidate + exact changes
    |
    v
DatastoreManager ---- running / candidate / startup / locks / commit
    |
    v
NetconfServer ------- unframed XML RPC processing and NACM
    |
    v
NetconfSession ------ RFC 6242 hello and message framing
    |
    v
Host application ---- authentication, streams, persistence, and device backend
```

### Compile the model

`yang::Compiler` is the normal entry point for YANG. It resolves imports and
includes through a `ModuleRepository`, then performs the semantic passes in the
required order. A compilation owns the resolved modules and effective schemas.
Diagnostics retain source locations and stable error codes suitable for a CLI
or editor integration.

The application converts a successful compilation into a
`yang::config::RuntimeSchema`. This compact, immutable schema is shared by
configuration parsing, validation, editing, filtering, authorization, and
change detection. Keep it alive for at least as long as those objects.

### Parse and validate configuration

`ParseDatastoreXml` binds XML elements to schema nodes by namespace URI, not by
the spelling of XML prefixes. It returns an immutable `ConfigDocument` or
structured findings. `ConfigValidator` then checks the bound tree, including
mandatory nodes, types, list keys, uniqueness, choices, references, `must`, and
`when` constraints.

Use complete validation for a datastore that represents the whole device.
Partial validation can distinguish an invalid fragment from one that cannot be
decided without additional context.

### Edit and transact

`ConfigEditor` applies NETCONF edit operations to an immutable document and
returns a replacement candidate plus exact, schema-associated `ChangeEvent`
objects. `DatastoreManager` builds the stateful NETCONF transaction model on
top: running, candidate, and startup datastores; locks; validation; commit and
discard; confirmed commits; and persistence snapshots.

A deterministic change list is useful for authorization, auditing, and backend
planning. Its path order is not a safe hardware execution order. A real device
backend must preflight platform limits and derive dependency-ordered actions;
see [Safe hardware application ordering](../dangd/README.md#safe-hardware-application-ordering).

Dangd retains the backend's accepted working configuration separately from the
protocol datastore objects. NMDA `<operational>` configuration is built from
that applied snapshot and then augmented with observed plugin and core state;
it does not blindly echo the server's running-tree input. ABI-v6 backends can
report a different schema-valid applied snapshot and per-node applied,
transformed, rejected, or delayed results. These appear under
`dangd-reconciliation`; unapplied requested values do not appear as device
state.

An ABI-v6 backend may also attach `ietf-origin:origin` to nodes in its returned
applied XML. Dangd validates the identity, preserves inheritance, and supports
positive and negated origin filters for standard and vendor-derived origins.
Metadata is emitted only when `<get-data>` requests `with-origin`.

### Serve NETCONF

`NetconfServer` accepts one complete, unframed `<rpc>` document and returns an
`<rpc-reply>`. It implements protocol operations over `DatastoreManager` and
can be given NACM policy, URL datastore, notification, and with-defaults
providers.

`NetconfSession` adds the server hello, NETCONF base 1.0 or 1.1 negotiation,
incremental RFC 6242 framing, session registration, and close/kill handling. It
does not itself open sockets or authenticate peers; those remain embedding-host
responsibilities. The included `dangd` host supplies an embedded libssh server
with explicit public-key authorization and an OpenSSL mutual-TLS server.

## Building the project

On macOS with Homebrew dependencies installed:

```sh
brew install cmake fmt libxml2 libssh pugixml nlohmann-json googletest
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Applications installed separately can use the exported CMake package:

```cmake
find_package(yang CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE yang::yang)
target_compile_features(my_application PRIVATE cxx_std_20)
```

## A complete `dangd` example

`dangd` demonstrates the host-application pattern. It compiles a root model,
loads and validates initial configuration, owns the datastores and protocol
server, optionally persists datastore state, and reports committed changes to
an in-memory backend in English. Initial, restored, and reload-provided running
configuration is activated through affected plugins before the application is
made available to a transport.

### 1. Create a model

Save this as `appliance.yang`:

```yang
module appliance {
  yang-version 1.1;
  namespace "urn:example:appliance";
  prefix a;

  container system {
    leaf hostname {
      type string;
      mandatory true;
    }
  }
}
```

For imported modules, place dependencies beside the root model or add one or
more `--search DIR` arguments. The resolver recognizes both `name.yang` and
`name@revision.yang`.

### 2. Create initial configuration

Save this as `config.xml`:

```xml
<config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <system xmlns="urn:example:appliance">
    <hostname>edge-1</hostname>
  </system>
</config>
```

The NETCONF wrapper is optional for the library parser, but using it makes the
file's purpose explicit.

### 3. Validate startup inputs

```sh
./build/dangd --model appliance.yang --config config.xml --check
```

Success produces:

```text
dangd: configuration is valid
```

If the hostname is omitted, startup fails before the server is constructed.
Validation findings identify the reason and namespace-qualified instance path.

### 4. Run a supervised NETCONF session

The following examples show the XML exchanged with `dangd`. Its `--stdio` mode
uses RFC 6242 framing, so an integration harness must append `]]>]]>` to base
1.0 hello and RPC messages. Start it with:

```sh
./build/dangd --model appliance.yang --config config.xml \
  --stdio --username operator --session-id 42
```

`dangd` first sends a server `<hello>`. Send a client hello that selects base
1.0:

```xml
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <capabilities>
    <capability>urn:ietf:params:netconf:base:1.0</capability>
  </capabilities>
</hello>]]>]]>
```

The username is trusted input in this demonstration mode. Do not expose
`--stdio` directly to a network; an authenticated local supervisor must own the
SSH or TLS connection and supply the verified identity.

### 5. Read the running configuration

Send:

```xml
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
  <get-config><source><running/></source></get-config>
</rpc>]]>]]>
```

The reply contains the current configuration under `<data>`:

```xml
<rpc-reply xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
  <data>
    <system xmlns="urn:example:appliance">
      <hostname>edge-1</hostname>
    </system>
  </data>
</rpc-reply>]]>]]>
```

Whitespace may differ; XML namespace and data semantics are significant.

### 6. Edit candidate and commit

Change the candidate hostname:

```xml
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="2">
  <edit-config>
    <target><candidate/></target>
    <config>
      <system xmlns="urn:example:appliance">
        <hostname>edge-2</hostname>
      </system>
    </config>
  </edit-config>
</rpc>]]>]]>
```

A successful edit returns `<ok/>`, but running and the backend remain at
`edge-1` until commit:

```xml
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="3">
  <commit/>
</rpc>]]>]]>
```

After the successful reply, `dangd` reports the backend delta on its diagnostic
stream:

```text
dangd: configuration delta: Changed /{urn:example:appliance}system/{urn:example:appliance}hostname from "edge-1" to "edge-2".
```

This demonstrates the commit boundary: the backend receives the complete
before and after documents plus exact changes whenever running is replaced.
The current backend only records them and swaps an in-memory document.

### 7. Observe a validation failure

An edit can be staged without validation by using `test-option` `set`. This
request deletes the mandatory hostname from candidate:

```xml
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="4">
  <edit-config>
    <target><candidate/></target>
    <test-option>set</test-option>
    <config>
      <system xmlns="urn:example:appliance">
        <hostname xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
                  nc:operation="delete">edge-2</hostname>
      </system>
    </config>
  </edit-config>
</rpc>]]>]]>
```

The edit succeeds, but a subsequent `<commit/>` is rejected with structured
diagnostic context:

```xml
<rpc-error>
  <error-type>application</error-type>
  <error-tag>missing-element</error-tag>
  <error-severity>error</error-severity>
  <error-path xmlns:n0="urn:example:appliance">/n0:system/n0:hostname</error-path>
  <error-message xml:lang="en">mandatory data node is absent (module: appliance, path: /{urn:example:appliance}system/{urn:example:appliance}hostname)</error-message>
</rpc-error>
```

Running and the backend remain unchanged, and no backend delta is emitted.
Send `<discard-changes/>` to restore candidate from running.

### 8. Persist datastore state

Add `--state FILE` to load an existing snapshot at startup and atomically save
persistent datastore state after session activity:

```sh
./build/dangd --model appliance.yang --config config.xml \
  --state appliance-state.json --stdio --username operator
```

Treat the snapshot as server-owned data. It is a persistence format for this
implementation, not a client-facing replacement for NETCONF configuration
encoding. The file contains the complete configuration, including modeled
password hashes or other secrets, and is created mode 0600. Startup refuses a
symbolic link, non-regular file, file owned by another effective user, or file
with any group/other permission bits.

Because each update atomically replaces the snapshot, an administrator may
copy it while dangd is running and will obtain either the complete previous or
complete current generation. Preserve private permissions on the backup:

```sh
install -m 600 appliance-state.json appliance-state.backup.json
```

To restore, stop dangd, retain the failed/current snapshot separately, install
the chosen backup at the configured `--state` path with mode 0600 and the
service identity as owner, then start dangd normally. Startup parses and
schema-validates every stored datastore, then activates the effective running
tree through affected plugins in dependency order before serving requests. It
fails closed if the snapshot is corrupt, incompatible, unsafe, or unreadable,
or if a plugin rejects or cannot apply the restored configuration. Do not edit
snapshot JSON manually; use NETCONF to migrate configuration between different
schemas or software versions.

### 9. Bootstrap and rotate privileged access

Dangd includes the recovery name `dangd-superuser`. It exists only inside
dangd's NACM enforcement and is not a Unix user. Installation creates no login,
password, home directory, private key, or certificate, so the name cannot log
in to SSH, PAM, or another application by itself.

For the embedded SSH server, explicitly bind a public key:

```sh
--ssh-authorized-key dangd-superuser=/secure/admin-key.pub
```

For mutual TLS, either issue a trusted client certificate whose selected
CN/SAN is exactly `dangd-superuser`, or use an exact mapping such as:

```sh
--username-map device-recovery-certificate=dangd-superuser
```

Every RPC attempted with this identity is written to dangd's recovery audit
stream with the session ID, safely encoded username, and request byte count.
Protect and retain the daemon diagnostic/audit output according to local
policy; no NETCONF payload or secret is included in this record.

Rotate the SSH key or TLS certificate in the transport configuration and use a
controlled restart or SIGHUP, then verify NETCONF recovery access with the new
credential before revoking the old one. For permanent removal, first configure
and test another `--recovery-user`, then add `--no-default-superuser` and reload.
The identity is absent immediately in the replacement application. There is no
operating-system account or stored dangd credential to delete. If all recovery
access is accidentally removed, stop the daemon and repair its launch options
or initial NACM input through the protected host console before restarting.

## Embedding the same pattern

The essential ownership pattern used by `dangd` is:

```cpp
auto compilation = compiler.Compile(source);
auto schema = yang::config::RuntimeSchemaBuilder::FromCompilation(*compilation);
auto parsed = yang::config::ParseDatastoreXml(schema, initial_xml);

yang::config::ConfigValidator validator;
auto validation = validator.Validate({schema, *parsed.document});
if (!validation.valid || !validation.complete) return startup_failure;

DeviceBackend backend(*parsed.document);
yang::netconf::DatastoreManager datastores(
    schema, std::move(*parsed.document), std::nullopt, &backend);
yang::netconf::NetconfServer server(datastores);
```

Production code must check every optional/result value and render all compiler
diagnostics and validation findings; the abbreviated example emphasizes object
relationships. The host then creates one `NetconfSession` per authenticated
connection, forwards received bytes to `Receive`, writes every returned byte
buffer, calls `Poll` for asynchronous work, and calls `TransportClosed` when
the connection ends.

The host is also responsible for:

- authenticating SSH or TLS peers and supplying stable usernames and groups;
- loading NACM policy and connecting operational providers;
- setting resource and message limits appropriate to the deployment;
- scheduling confirmed-commit expiry and notification delivery;
- persisting state and protecting state files;
- translating desired configuration into safe device operations; and
- reporting and recovering from backend application or rollback failures.

The last item is intentionally not claimed by the current `RunningConfigBackend`
interface. `dangd` is a working protocol and integration example, not yet a
production hardware agent.

For a directly usable secure demonstration, follow the
[mutual-TLS console example](../dangd/README.md#mutual-tls-console-example).
It connects the included `dangctl` paste-and-reply client to `dangd`, maps a
verified client certificate identity into NACM, and clearly identifies the
test-only trust material and production limitations.

## Where to go next

- [Configuration validation](CONFIG_VALIDATION.md)
- [Configuration editing](CONFIG_EDIT.md)
- [NETCONF datastores](NETCONF_DATASTORE.md)
- [NETCONF server](NETCONF_SERVER.md)
- [NETCONF framing and sessions](NETCONF_FRAMING.md)
- [Transport integration](NETCONF_TRANSPORT.md)
- [Filtering, NACM, and persistence](NETCONF_FILTER_NACM_PERSISTENCE.md)
- [Writing a dangd configuration plugin](DANGD_PLUGINS.md)
- [`dangd` design and limitations](../dangd/README.md)
