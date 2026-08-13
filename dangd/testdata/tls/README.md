<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Test-only TLS credentials

These certificates and unencrypted private keys exist only for deterministic
loopback tests and the `dangd`/`dangctl` walkthrough. They are public repository
fixtures. Never install their CA as trusted, reuse their keys, or deploy them.

The CA signs:

- `server-cert.pem`, valid for `localhost` and `127.0.0.1`, with server-auth
  usage; and
- `alice-cert.pem`, whose common name is `alice`, with client-auth usage.

`dangd` verifies the client chain and maps that common name to the NETCONF
username. `dangctl` verifies the server chain and hostname.
