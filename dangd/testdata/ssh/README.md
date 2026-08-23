<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Test SSH keys

These unencrypted Ed25519 keys exist only for the loopback SSH integration
test. They are public repository fixtures and must never be installed or used
by a deployed `dangd` server.

- `host-key` is the test server host private key.
- `alice-key` is the authorized test client private key.
- The matching `.pub` files are used for explicit server authorization.
