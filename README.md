<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# dangd NETCONF server

`dangd` is a model-driven NETCONF configuration server. It loads YANG models,
validates device configuration, exposes standard NETCONF datastores, enforces
NACM access control, persists configuration, and coordinates device plugins so
configuration is published only after the affected hardware accepts it.

The repository also contains the reusable C++20 YANG and NETCONF library that
supports the daemon, plus `yangc` for model validation and `dangctl` for an
interactive mutual-TLS NETCONF session.

The project is under active development. Consult the
[remaining work](TODO.md) and [standards compliance ledger](docs/COMPLIANCE.md)
before treating it as a production-ready or fully conforming implementation.

## What dangd provides

- NETCONF running, candidate, startup, intended, and operational datastores;
- SSH public-key and mutual-TLS authenticated sessions;
- datastore-managed RFC 8341 NACM authorization and recovery controls;
- RFC 8525 YANG Library inventory, model retrieval, and atomic `SIGHUP` reload;
- confirmed commits, rollback, locks, notifications, filters, and
  with-defaults behavior;
- persistent datastore snapshots with recovery after restart;
- dynamically loaded, supervised plugins that supply models, validate changes,
  apply dependency-ordered hardware actions, compensate failures, and publish
  operational state; and
- an RFC 8343/8344 IP-management example with native Linux and FreeBSD backend
  directories.

The [user guide](docs/USER_GUIDE.md) explains the architecture and walks through
a complete configuration session. The [dangd guide](dangd/README.md) covers
deployment modes, security expectations, hardware transaction ordering, NACM,
and plugins in detail.

## Build

On macOS with Homebrew:

```sh
brew install cmake fmt libxml2 libssh pugixml nlohmann-json googletest
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Linux and FreeBSD are supported server targets. Windows support is not a
project goal.

## Validate a device configuration

The repository includes a small appliance model and configuration:

```sh
./build/dangd \
  --model dangd/examples/appliance.yang \
  --config dangd/examples/config.xml \
  --search dangd/models \
  --check
```

Success means the model dependency closure compiled and the complete initial
configuration passed schema validation. Startup errors include the module and
instance path where possible.

## Try NETCONF over mutual TLS

The bundled certificates are public test fixtures and must never be deployed.
Start the local server:

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

In another terminal, connect with the deliberately simple interactive client:

```sh
./build/dangctl \
  --host localhost --port 6513 \
  --cert dangd/testdata/tls/alice-cert.pem \
  --key dangd/testdata/tls/alice-key.pem \
  --ca dangd/testdata/tls/ca-cert.pem
```

Paste one XML RPC and then enter a blank line. For example:

```xml
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
  <get-config><source><running/></source></get-config>
</rpc>
```

The client frames the request and displays the decoded reply. SSH and
supervised stdio examples are in the [dangd guide](dangd/README.md).

## Device plugins

Plugins declare the YANG modules they implement and participate in a two-phase
configuration transaction. All affected plugins first receive the proposed
tree and validate their portion against the same dependency-complete snapshot.
Only then does `dangd` apply the globally ordered hardware plan. A failure
compensates completed actions in reverse order and prevents the running
datastore from advancing.

The included IP-management plugin demonstrates RFC 8343 and RFC 8344 model
discovery, configuration parsing, native platform application, exact rollback,
and live Linux link, address, and neighbor state. See the
[plugin author guide](docs/DANGD_PLUGINS.md) for the ABI contract and practical
implementation expectations.

## Documentation map

- [User guide](docs/USER_GUIDE.md): concepts and an end-to-end `dangd` example
- [dangd guide](dangd/README.md): server operation, transports, NACM, plugins,
  persistence, reload, and hardware safety
- [Plugin author guide](docs/DANGD_PLUGINS.md): lifecycle and backend contract
- [YANG library guide](docs/YANG_LIBRARY.md): `yangc` and reusable C++ APIs
- [Security hardening](docs/HARDENING.md) and
  [XML security](docs/XML_SECURITY.md)
- [Standards compliance](docs/COMPLIANCE.md) and
  [NACM compliance matrix](docs/NACM_COMPLIANCE_MATRIX.md)
- [Release and documentation builds](docs/RELEASING.md)
- [Changelog](CHANGELOG.md) and [remaining work](TODO.md)

Copyright 2026 David Cornejo. Licensed under the Apache License, Version 2.0.
See [LICENSE](LICENSE) and [NOTICE](NOTICE).
